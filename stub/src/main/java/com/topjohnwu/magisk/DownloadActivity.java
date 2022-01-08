package com.topjohnwu.magisk;

import static android.R.string.no;
import static android.R.string.ok;
import static android.R.string.yes;
import static com.topjohnwu.magisk.R2.string.dling;
import static com.topjohnwu.magisk.R2.string.no_internet_msg;
import static com.topjohnwu.magisk.R2.string.relaunch_app;
import static com.topjohnwu.magisk.R2.string.upgrade_msg;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.content.Context;
import android.content.Intent;
import android.os.AsyncTask;
import android.os.Bundle;
import android.util.Log;
import android.view.ContextThemeWrapper;
import android.widget.Toast;

import com.topjohnwu.magisk.net.Networking;
import com.topjohnwu.magisk.net.Request;
import com.topjohnwu.magisk.utils.APKInstall;

import org.json.JSONException;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.GZIPInputStream;

import javax.crypto.Cipher;
import javax.crypto.CipherInputStream;
import javax.crypto.SecretKey;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

import io.michaelrocks.paranoid.Obfuscate;

@Obfuscate
public class DownloadActivity extends Activity {

    private static final String APP_NAME = "Magisk";
    private static final String CANARY_URL = "https://raw.githubusercontent.com/TheHitMan7/Magisk-Files/master/configs/canary.json";

    private String apkLink = BuildConfig.APK_URL;
    private Context themed;
    private ProgressDialog dialog;
    private boolean dynLoad;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        themed = new ContextThemeWrapper(this, android.R.style.Theme_DeviceDefault);

        // Only download and dynamic load full APK if hidden
        dynLoad = getPackageName().equals(BuildConfig.APPLICATION_ID);

        // Inject resources
        loadResources();

        if (Networking.checkNetworkStatus(this)) {
            if (apkLink == null) {
                fetchCanary();
            } else {
                showDialog();
            }
        } else {
            new AlertDialog.Builder(themed)
                    .setCancelable(false)
                    .setTitle(APP_NAME)
                    .setMessage(getString(no_internet_msg))
                    .setNegativeButton(ok, (d, w) -> finish())
                    .show();
        }
    }

    private void error(Throwable e) {
        Log.e(getClass().getSimpleName(), "", e);
        Toast.makeText(themed, e.getMessage(), Toast.LENGTH_LONG).show();
        finish();
    }

    private Request request(String url) {
        return Networking.get(url).setErrorHandler((conn, e) -> error(e));
    }

    private void showDialog() {
        new AlertDialog.Builder(themed)
                .setCancelable(false)
                .setTitle(APP_NAME)
                .setMessage(getString(upgrade_msg))
                .setPositiveButton(yes, (d, w) -> dlAPK())
                .setNegativeButton(no, (d, w) -> finish())
                .show();
    }

    private void fetchCanary() {
        dialog = ProgressDialog.show(themed, "", "", true);
        request(CANARY_URL).getAsJSONObject(json -> {
            dialog.dismiss();
            try {
                apkLink = json.getJSONObject("magisk").getString("link");
                showDialog();
            } catch (JSONException e) {
                error(e);
            }
        });
    }

    private void dlAPK() {
        dialog = ProgressDialog.show(themed, getString(dling), getString(dling) + " " + APP_NAME, true);
        Runnable onSuccess = () -> {
            dialog.dismiss();
            Toast.makeText(themed, relaunch_app, Toast.LENGTH_LONG).show();
            finish();
        };
        // Download and upgrade the app
        File apk = dynLoad ? DynAPK.current(this) : new File(getCacheDir(), "manager.apk");
        request(apkLink).setExecutor(AsyncTask.THREAD_POOL_EXECUTOR).getAsFile(apk, file -> {
            if (dynLoad) {
                DynLoad.setup(this);
                onSuccess.run();
            } else {
                var receiver = APKInstall.register(this, BuildConfig.APPLICATION_ID, onSuccess);
                APKInstall.installapk(this, file);
                Intent intent = receiver.waitIntent();
                if (intent != null) startActivity(intent);
            }
        });
    }

    private void loadResources() {
        File apk = new File(getCacheDir(), "res.apk");
        try {
            Cipher cipher = Cipher.getInstance("AES/CBC/PKCS5Padding");
            SecretKey key = new SecretKeySpec(Bytes.key(), "AES");
            IvParameterSpec iv = new IvParameterSpec(Bytes.iv());
            cipher.init(Cipher.DECRYPT_MODE, key, iv);
            InputStream is = new CipherInputStream(new ByteArrayInputStream(Bytes.res()), cipher);
            try (InputStream gzip = new GZIPInputStream(is);
                 OutputStream out = new FileOutputStream(apk)) {
                APKInstall.transfer(gzip, out);
            }
            DynAPK.addAssetPath(getResources().getAssets(), apk.getPath());
        } catch (Exception ignored) {
        }
    }

}
