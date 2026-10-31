/* 自动生成：tools/gen_missing.py（只补 ROM 有、桥缺的 native） */
#ifndef TVM_GEN_MISSING_H
#define TVM_GEN_MISSING_H

static const JNINativeMethod kMiss_android_os_Trace[] = {
    { "nativeAsyncTraceEnd", "(JLjava/lang/String;I)V", (void*)tvm_stub_void },
    { "nativeAsyncTraceForTrackBegin", "(JLjava/lang/String;Ljava/lang/String;I)V", (void*)tvm_stub_void },
    { "nativeAsyncTraceForTrackEnd", "(JLjava/lang/String;I)V", (void*)tvm_stub_void },
    { "nativeInstant", "(JLjava/lang/String;)V", (void*)tvm_stub_void },
    { "nativeInstantForTrack", "(JLjava/lang/String;Ljava/lang/String;)V", (void*)tvm_stub_void },
    { "nativeSetAppTracingAllowed", "(Z)V", (void*)tvm_stub_void },
    { "nativeSetTracingEnabled", "(Z)V", (void*)tvm_stub_void },
    { "nativeTraceCounter", "(JLjava/lang/String;J)V", (void*)tvm_stub_void },
};

static const JNINativeMethod kMiss_android_os_SystemClock[] = {
    { "currentTimeMicro", "()J", (void*)tvm_stub_one },
};

static const JNINativeMethod kMiss_android_os_Binder[] = {
    { "blockUntilThreadAvailable", "()V", (void*)tvm_stub_void },
    { "clearCallingIdentity", "()J", (void*)tvm_stub_one },
    { "clearCallingWorkSource", "()J", (void*)tvm_stub_one },
    { "flushPendingCommands", "()V", (void*)tvm_stub_void },
    { "forceDowngradeToSystemStability", "()V", (void*)tvm_stub_void },
    { "getCallingWorkSourceUid", "()I", (void*)tvm_stub_int },
    { "getThreadStrictModePolicy", "()I", (void*)tvm_stub_int },
    { "hasExplicitIdentity", "()Z", (void*)tvm_stub_false },
    { "isDirectlyHandlingTransactionNative", "()Z", (void*)tvm_stub_false },
    { "markVintfStability", "()V", (void*)tvm_stub_void },
    { "restoreCallingIdentity", "(J)V", (void*)tvm_stub_void },
    { "restoreCallingWorkSource", "(J)V", (void*)tvm_stub_void },
    { "setCallingWorkSourceUid", "(I)J", (void*)tvm_stub_one },
    { "setExtensionNative", "(Landroid/os/IBinder;)V", (void*)tvm_stub_void },
    { "setThreadStrictModePolicy", "(I)V", (void*)tvm_stub_void },
};

static const JNINativeMethod kMiss_com_android_internal_os_BinderInternal[] = {
    { "disableBackgroundScheduling", "(Z)V", (void*)tvm_stub_void },
    { "handleGc", "()V", (void*)tvm_stub_void },
    { "joinThreadPool", "()V", (void*)tvm_stub_void },
    { "nGetBinderProxyCount", "(I)I", (void*)tvm_stub_int },
    { "nGetBinderProxyPerUidCounts", "()Landroid/util/SparseIntArray;", (void*)tvm_stub_null },
    { "nSetBinderProxyCountEnabled", "(Z)V", (void*)tvm_stub_void },
    { "nSetBinderProxyCountWatermarks", "(III)V", (void*)tvm_stub_void },
    { "setMaxThreads", "(I)V", (void*)tvm_stub_void },
};

static const JNINativeMethod kMiss_android_util_EventLog[] = {
    { "readEvents", "([ILjava/util/Collection;)V", (void*)tvm_stub_void },
    { "readEventsOnWrapping", "([IJLjava/util/Collection;)V", (void*)tvm_stub_void },
};

static const JNINativeMethod kMiss_android_os_MessageQueue[] = {
    { "nativeIsPolling", "(J)Z", (void*)tvm_stub_false },
};

static const JNINativeMethod kMiss_android_content_res_AssetManager[] = {
    { "getAssetAllocations", "()Ljava/lang/String;", (void*)tvm_stub_null },
    { "getGlobalAssetCount", "()I", (void*)tvm_stub_int },
    { "getGlobalAssetManagerCount", "()I", (void*)tvm_stub_int },
    { "nativeApplyStyleWithArray", "(JJIIJ[I[I[I)V", (void*)tvm_stub_void },
    { "nativeAttributeResolutionStack", "(JJIII)[I", (void*)tvm_stub_null },
    { "nativeGetAssignedPackageIdentifiers", "(JZZ)Landroid/util/SparseArray;", (void*)tvm_stub_null },
    { "nativeGetResourceBagValue", "(JIILandroid/util/TypedValue;)I", (void*)tvm_stub_int },
    { "nativeGetSizeAndUiModeConfigurations", "(J)[Landroid/content/res/Configuration;", (void*)tvm_stub_null },
    { "nativeOpenXmlAssetFd", "(JILjava/io/FileDescriptor;)J", (void*)tvm_stub_one },
    { "nativeSetConfiguration", "(JIILjava/lang/String;[Ljava/lang/String;IIIIIIIIIIIIIIIIZ)V", (void*)tvm_stub_void },
    { "nativeSetOverlayConstraints", "(JII)V", (void*)tvm_stub_void },
    { "nativeThemeRebase", "(JJ[I[ZI)V", (void*)tvm_stub_void },
};

static const JNINativeMethod kMiss_android_graphics_Typeface[] = {
    { "nativeIsVariationInstance", "(J)Z", (void*)tvm_stub_false },
};

#endif
