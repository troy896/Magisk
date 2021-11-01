#include <csignal>
#include <libgen.h>
#include <sys/un.h>
#include <sys/mount.h>

#include <magisk.hpp>
#include <utils.hpp>
#include <daemon.hpp>
#include <selinux.hpp>
#include <db.hpp>
#include <resetprop.hpp>
#include <flags.h>

#include "core.hpp"

using namespace std;

int SDK_INT = -1;
string MAGISKTMP;

bool RECOVERY_MODE = false;
int DAEMON_STATE = STATE_NONE;

static struct stat self_st;

static map<int, poll_callback> *poll_map;
static vector<pollfd> *poll_fds;
static int poll_ctrl;

enum {
    POLL_CTRL_NEW,
    POLL_CTRL_RM,
};

void register_poll(const pollfd *pfd, poll_callback callback) {
    if (gettid() == getpid()) {
        // On main thread, directly modify
        poll_map->try_emplace(pfd->fd, callback);
        poll_fds->emplace_back(*pfd);
    } else {
        // Send it to poll_ctrl
        write_int(poll_ctrl, POLL_CTRL_NEW);
        xwrite(poll_ctrl, pfd, sizeof(*pfd));
        xwrite(poll_ctrl, &callback, sizeof(callback));
    }
}

void unregister_poll(int fd, bool auto_close) {
    if (gettid() == getpid()) {
        // On main thread, directly modify
        poll_map->erase(fd);
        for (auto &poll_fd : *poll_fds) {
            if (poll_fd.fd == fd) {
                if (auto_close) {
                    close(poll_fd.fd);
                }
                // Cannot modify while iterating, invalidate it instead
                // It will be removed in the next poll loop
                poll_fd.fd = -1;
                break;
            }
        }
    } else {
        // Send it to poll_ctrl
        write_int(poll_ctrl, POLL_CTRL_RM);
        write_int(poll_ctrl, fd);
        write_int(poll_ctrl, auto_close);
    }
}

static void poll_ctrl_handler(pollfd *pfd) {
    int code = read_int(pfd->fd);
    switch (code) {
        case POLL_CTRL_NEW: {
            pollfd new_fd;
            poll_callback cb;
            xxread(pfd->fd, &new_fd, sizeof(new_fd));
            xxread(pfd->fd, &cb, sizeof(cb));
            register_poll(&new_fd, cb);
            break;
        }
        case POLL_CTRL_RM: {
            int fd = read_int(pfd->fd);
            bool auto_close = read_int(pfd->fd);
            unregister_poll(fd, auto_close);
            break;
        }
    }
}

[[noreturn]] static void poll_loop() {
    // Register poll_ctrl
    int pipefd[2];
    xpipe2(pipefd, O_CLOEXEC);
    poll_ctrl = pipefd[1];
    pollfd poll_ctrl_pfd = { pipefd[0], POLLIN, 0 };
    register_poll(&poll_ctrl_pfd, poll_ctrl_handler);

    for (;;) {
        if (poll(poll_fds->data(), poll_fds->size(), -1) <= 0)
            continue;

        // MUST iterate with index because any poll_callback could add new elements to poll_fds
        for (int i = 0; i < poll_fds->size();) {
            auto &pfd = (*poll_fds)[i];
            if (pfd.revents) {
                if (pfd.revents & POLLERR || pfd.revents & POLLNVAL) {
                    poll_map->erase(pfd.fd);
                    poll_fds->erase(poll_fds->begin() + i);
                    continue;
                }
                if (auto it = poll_map->find(pfd.fd); it != poll_map->end()) {
                    it->second(&pfd);
                }
            }
            ++i;
        }
    }
}

static bool verify_client(pid_t pid) {
    // Verify caller is the same as server
    char path[32];
    sprintf(path, "/proc/%d/exe", pid);
    struct stat st;
    return !(stat(path, &st) || st.st_dev != self_st.st_dev || st.st_ino != self_st.st_ino);
}

static bool check_zygote(pid_t pid) {
    char buf[32];
    sprintf(buf, "/proc/%d/attr/current", pid);
    if (auto fp = open_file(buf, "r")) {
        fscanf(fp.get(), "%s", buf);
        return buf == "u:r:zygote:s0"sv;
    } else {
        return false;
    }
}

static void handle_request_async(int client, int code, ucred cred) {
    switch (code) {
    case MAGISKHIDE:
        magiskhide_handler(client, &cred);
        break;
    case SUPERUSER:
        su_daemon_handler(client, &cred);
        break;
    case POST_FS_DATA:
        post_fs_data(client);
        break;
    case LATE_START:
        late_start(client);
        break;
    case BOOT_COMPLETE:
        boot_complete(client);
        break;
    case SQLITE_CMD:
        exec_sql(client);
        break;
    case REMOVE_MODULES:
        remove_modules();
        write_int(client, 0);
        close(client);
        reboot();
        break;
    case ZYGISK_REQUEST:
        zygisk_handler(client, &cred);
        break;
    default:
        close(client);
        break;
    }
}

static void handle_request_sync(int client, int code) {
    switch (code) {
    case CHECK_VERSION:
        write_string(client, MAGISK_VERSION ":MAGISK");
        break;
    case CHECK_VERSION_CODE:
        write_int(client, MAGISK_VER_CODE);
        break;
    case GET_PATH:
        write_string(client, MAGISKTMP.data());
        break;
    case START_DAEMON:
        setup_logfile(true);
        break;
    case STOP_DAEMON:
        magiskhide_handler(-1, nullptr);
        write_int(client, 0);
        // Terminate the daemon!
        exit(0);
    }
}

static void handle_request(pollfd *pfd) {
    int client = xaccept4(pfd->fd, nullptr, nullptr, SOCK_CLOEXEC);

    // Verify client credentials
    ucred cred;
    get_client_cred(client, &cred);

    bool is_root = cred.uid == UID_ROOT;
    bool is_client = verify_client(cred.pid);
    bool is_zygote = !is_client && check_zygote(cred.pid);
    int code;

    if (!is_root && !is_zygote && !is_client)
        goto done;

    code = read_int(client);
    if (code < 0 || (code & DAEMON_CODE_MASK) >= DAEMON_CODE_END)
        goto done;

    // Check client permissions
    switch (code) {
    case POST_FS_DATA:
    case LATE_START:
    case BOOT_COMPLETE:
    case SQLITE_CMD:
    case GET_PATH:
        if (!is_root) {
            write_int(client, ROOT_REQUIRED);
            goto done;
        }
        break;
    case REMOVE_MODULES:
        if (!is_root && cred.uid != UID_SHELL) {
            write_int(client, 1);
            goto done;
        }
        break;
    case MAGISKHIDE:  // accept hide request from zygote
        if (!is_root && !is_zygote) {
            write_int(client, ROOT_REQUIRED);
            goto done;
        }
        break;
    case STOP_DAEMON:
        if (!is_root) {
            write_int(client, ROOT_REQUIRED);
            goto done;
        }
        break;
    case ZYGISK_REQUEST:
        if (!is_zygote) {
            write_int(client, DAEMON_ERROR);
            goto done;
        }
        break;
    }

    if (code & SYNC_FLAG) {
        handle_request_sync(client, code);
        goto done;
    }

    // Handle complex requests in another thread
    exec_task([=] { handle_request_async(client, code, cred); });
    return;

done:
    close(client);
}

static int switch_cgroup(const char *cgroup, int pid) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%s/cgroup.procs", cgroup);
    int fd = open(buf, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd == -1)
        return -1;
    snprintf(buf, sizeof(buf), "%d\n", pid);
    if (xwrite(fd, buf, strlen(buf)) == -1) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void daemon_entry() {
    magisk_logging();

    // Block all signals
    sigset_t block_set;
    sigfillset(&block_set);
    pthread_sigmask(SIG_SETMASK, &block_set, nullptr);

    // Change process name
    set_nice_name("magiskd");

    int fd = xopen("/dev/null", O_WRONLY);
    xdup2(fd, STDOUT_FILENO);
    xdup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
        close(fd);
    fd = xopen("/dev/zero", O_RDONLY);
    xdup2(fd, STDIN_FILENO);
    if (fd > STDERR_FILENO)
        close(fd);

    setsid();
    setcon("u:r:" SEPOL_PROC_DOMAIN ":s0");

    start_log_daemon();

    LOGI(NAME_WITH_VER(Magisk) " daemon started\n");

    // Escape from cgroup
    int pid = getpid();
    if (switch_cgroup("/acct", pid) && switch_cgroup("/sys/fs/cgroup", pid))
        LOGW("Can't switch cgroup\n");

    // Get self stat
    char buf[64];
    xreadlink("/proc/self/exe", buf, sizeof(buf));
    MAGISKTMP = dirname(buf);
    xstat("/proc/self/exe", &self_st);

    // Get API level
    parse_prop_file("/system/build.prop", [](auto key, auto val) -> bool {
        if (key == "ro.build.version.sdk") {
            SDK_INT = parse_int(val);
            return false;
        }
        return true;
    });
    if (SDK_INT < 0) {
        // In case some devices do not store this info in build.prop, fallback to getprop
        auto sdk = getprop("ro.build.version.sdk");
        if (!sdk.empty()) {
            SDK_INT = parse_int(sdk);
        }
    }
    LOGI("* Device API level: %d\n", SDK_INT);

    restore_tmpcon();

    // SAR cleanups
    auto mount_list = MAGISKTMP + "/" ROOTMNT;
    if (access(mount_list.data(), F_OK) == 0) {
        file_readline(true, mount_list.data(), [](string_view line) -> bool {
            umount2(line.data(), MNT_DETACH);
            return true;
        });
    }
    unlink("/dev/.se");
    unlink(mount_list.data());

    // Load config status
    auto config = MAGISKTMP + "/" INTLROOT "/config";
    parse_prop_file(config.data(), [](auto key, auto val) -> bool {
        if (key == "RECOVERYMODE" && val == "true")
            RECOVERY_MODE = true;
        return true;
    });

    // Use isolated devpts if kernel support
    if (access("/dev/pts/ptmx", F_OK) == 0) {
        auto pts = MAGISKTMP + "/" SHELLPTS;
        if (access(pts.data(), F_OK)) {
            xmkdirs(pts.data(), 0755);
            xmount("devpts", pts.data(), "devpts",
                   MS_NOSUID | MS_NOEXEC, "newinstance");
            auto ptmx = pts + "/ptmx";
            if (access(ptmx.data(), F_OK)) {
                xumount(pts.data());
                rmdir(pts.data());
            }
        }
    }

    sockaddr_un sun{};
    socklen_t len = setup_sockaddr(&sun, MAIN_SOCKET);
    fd = xsocket(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (xbind(fd, (sockaddr*) &sun, len))
        exit(1);
    xlisten(fd, 10);

    default_new(poll_map);
    default_new(poll_fds);

    // Register handler for main socket
    pollfd main_socket_pfd = { fd, POLLIN, 0 };
    register_poll(&main_socket_pfd, handle_request);

    // Loop forever to listen for requests
    poll_loop();
}

int connect_daemon(bool create) {
    sockaddr_un sun{};
    socklen_t len = setup_sockaddr(&sun, MAIN_SOCKET);
    int fd = xsocket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connect(fd, (sockaddr*) &sun, len)) {
        if (!create || getuid() != UID_ROOT) {
            LOGE("No daemon is currently running!\n");
            close(fd);
            return -1;
        }

        if (fork_dont_care() == 0) {
            close(fd);
            daemon_entry();
        }

        while (connect(fd, (struct sockaddr*) &sun, len))
            usleep(10000);
    }
    return fd;
}
