package com.topjohnwu.magisk;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AppComponentFactory;
import android.app.Application;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.ContentProvider;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.os.Process;

@SuppressLint("NewApi")
public class DelegateComponentFactory extends AppComponentFactory {

    AppComponentFactory receiver;

    public DelegateComponentFactory() {
        DynLoad.componentFactory = this;
    }

    @Override
    public ClassLoader instantiateClassLoader(ClassLoader cl, ApplicationInfo info) {
        if (Process.myUid() == 0) {
            // Do not do anything in root process
            return cl;
        }
        DynLoad.loadApk(info);
        return new DelegateClassLoader();
    }

    @Override
    public Application instantiateApplication(ClassLoader cl, String className) {
        return new DelegateApplication();
    }

    @Override
    public Activity instantiateActivity(ClassLoader cl, String className, Intent intent)
            throws ClassNotFoundException, IllegalAccessException, InstantiationException {
        if (receiver != null)
            return receiver.instantiateActivity(DynLoad.loader, className, intent);
        return create(className);
    }

    @Override
    public BroadcastReceiver instantiateReceiver(ClassLoader cl, String className, Intent intent)
            throws ClassNotFoundException, IllegalAccessException, InstantiationException {
        if (receiver != null)
            return receiver.instantiateReceiver(DynLoad.loader, className, intent);
        return create(className);
    }

    @Override
    public Service instantiateService(ClassLoader cl, String className, Intent intent)
            throws ClassNotFoundException, IllegalAccessException, InstantiationException {
        if (receiver != null)
            return receiver.instantiateService(DynLoad.loader, className, intent);
        return create(className);
    }

    @Override
    public ContentProvider instantiateProvider(ClassLoader cl, String className)
            throws ClassNotFoundException, IllegalAccessException, InstantiationException {
        if (receiver != null)
            return receiver.instantiateProvider(DynLoad.loader, className);
        return create(className);
    }

    private <T> T create(String name)
            throws ClassNotFoundException, IllegalAccessException, InstantiationException{
        return (T) DynLoad.loader.loadClass(name).newInstance();
    }

}
