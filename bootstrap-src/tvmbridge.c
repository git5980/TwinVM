/* TwinVM M3-b native 桥 v3：framework native 最小实现（23:36 案三连补）
 * onPause: Trace.nativeIsTagEnabled / onStop: SystemClock.elapsedRealtime */
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

/* ── android.util.Log ── */
static JNIEXPORT jboolean JNICALL
b_isLoggable(JNIEnv* env, jclass c, jstring t, jint l) {
    (void)env; (void)c; (void)t; (void)l; return JNI_FALSE;
}
static JNIEXPORT jint JNICALL
b_println_native(JNIEnv* env, jclass c, jint buf, jint prio, jstring tag, jstring msg) {
    (void)c; (void)buf; (void)prio;
    const char* t = tag ? (*env)->GetStringUTFChars(env, tag, NULL) : "TVM";
    const char* m = msg ? (*env)->GetStringUTFChars(env, msg, NULL) : "";
    int n = (int)strlen(m);
    fprintf(stdout, "[LC] %s: %s\n", t, m); fflush(stdout);
    if (msg) (*env)->ReleaseStringUTFChars(env, msg, m);
    if (tag && t) (*env)->ReleaseStringUTFChars(env, tag, t);
    return n;
}
static JNIEXPORT jint JNICALL
b_logger_max(JNIEnv* env, jclass c) { (void)env; (void)c; return 4068; }

/* ── android.os.SystemProperties ── */
static JNIEXPORT jboolean JNICALL
sp_get_boolean(JNIEnv* env, jclass c, jstring k, jboolean d) {
    (void)env; (void)c; (void)k; return d;
}
static JNIEXPORT jstring JNICALL
sp_get(JNIEnv* env, jclass c, jstring k, jstring d) {
    (void)c; (void)k; return d ? (jstring)(*env)->NewLocalRef(env, d) : NULL;
}
static JNIEXPORT jint JNICALL
sp_get_int(JNIEnv* env, jclass c, jstring k, jint d) {
    (void)env; (void)c; (void)k; return d;
}
static JNIEXPORT jlong JNICALL
sp_get_long(JNIEnv* env, jclass c, jstring k, jlong d) {
    (void)env; (void)c; (void)k; return d;
}
/* v18 ★ ROM API36 双 API：native_find/native_get(long handle) 族
 * （旧桥只有 String 版；SystemProperties.get 新链会走 handle 版） */
static JNIEXPORT jlong JNICALL
sp_find(JNIEnv* env, jclass c, jstring k) { (void)env; (void)c; (void)k; return 1; }
static JNIEXPORT jstring JNICALL
sp_get_handle(JNIEnv* env, jclass c, jlong h) {
    (void)c; (void)h; return (*env)->NewStringUTF(env, "x");   /* 非空（Build 解析安全） */
}
static JNIEXPORT jboolean JNICALL
sp_get_boolean_h(JNIEnv* env, jclass c, jlong h, jboolean d) { (void)env; (void)c; (void)h; return d; }
static JNIEXPORT jint JNICALL
sp_get_int_h(JNIEnv* env, jclass c, jlong h, jint d) { (void)env; (void)c; (void)h; return d; }
static JNIEXPORT jlong JNICALL
sp_get_long_h(JNIEnv* env, jclass c, jlong h, jlong d) { (void)env; (void)c; (void)h; return d; }
static JNIEXPORT void JNICALL
sp_change_cb(JNIEnv* env, jclass c) { (void)env; (void)c; }
static JNIEXPORT void JNICALL
sp_report(JNIEnv* env, jclass c) { (void)env; (void)c; }

static JNIEXPORT jstring JNICALL
sp_get_str(JNIEnv* env, jclass c, jstring k) {
    (void)c; (void)k;
    /* v57 ★：Build.<clinit>:430 案——空串导致 split()[0] 越界。
     * 返回占位非空串（Build 的字段解析就绪）。 */
    return (*env)->NewStringUTF(env, "x");
}
static JNIEXPORT void JNICALL
sp_set(JNIEnv* env, jclass c, jstring k, jstring v) { (void)env; (void)c; (void)k; (void)v; }
static JNIEXPORT void JNICALL
sp_add(JNIEnv* env, jclass c, jstring k, jstring v) { (void)env; (void)c; (void)k; (void)v; }

/* ── v3 ★ android.os.Trace（23:36 onPause 案）── */
static JNIEXPORT jboolean JNICALL
tr_isTagEnabled(JNIEnv* env, jclass c, jlong tag) {
    (void)env; (void)c; (void)tag; return JNI_FALSE;
}
static JNIEXPORT void JNICALL
tr_nativeTraceBegin(JNIEnv* env, jclass c, jlong tag, jstring name) {
    (void)env; (void)c; (void)tag; (void)name;
}
static JNIEXPORT void JNICALL
tr_nativeTraceEnd(JNIEnv* env, jclass c, jlong tag) { (void)env; (void)c; (void)tag; }
static JNIEXPORT void JNICALL
tr_nativeAsyncTraceBegin(JNIEnv* env, jclass c, jlong tag, jstring name, jint cookie) {
    (void)env; (void)c; (void)tag; (void)name; (void)cookie;
}
static JNIEXPORT void JNICALL
tr_nativeAsyncTraceEnd(JNIEnv* env, jclass c, jlong tag, jint cookie) {
    (void)env; (void)c; (void)tag; (void)cookie;
}
static JNIEXPORT void JNICALL
tr_nativeTraceCounter(JNIEnv* env, jclass c, jlong tag, jstring name, jint val) {
    (void)env; (void)c; (void)tag; (void)name; (void)val;
}

/* ── v3 ★ android.os.SystemClock（23:36 onStop 案）── */
static jlong now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static jlong uptime_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static JNIEXPORT jlong JNICALL
sc_elapsedRealtime(JNIEnv* env, jclass c) { (void)env; (void)c; return now_ms(); }
static JNIEXPORT jlong JNICALL
sc_elapsedRealtimeNanos(JNIEnv* env, jclass c) { (void)env; (void)c; return now_ms() * 1000000; }
static JNIEXPORT jlong JNICALL
sc_uptimeMillis(JNIEnv* env, jclass c) { (void)env; (void)c; return uptime_ms(); }
static JNIEXPORT jlong JNICALL
sc_currentThreadTimeMillis(JNIEnv* env, jclass c) { (void)env; (void)c; return uptime_ms(); }
static JNIEXPORT jlong JNICALL
sc_currentThreadTimeMicro(JNIEnv* env, jclass c) { (void)env; (void)c; return uptime_ms() * 1000; }
static JNIEXPORT jboolean JNICALL
sc_isCurrentThreadCpuTimeMonotonic(JNIEnv* env, jclass c) { (void)env; (void)c; return JNI_FALSE; }
static JNIEXPORT void JNICALL
sc_sleep(JNIEnv* env, jclass c, jlong ms) { (void)env; (void)c; usleep((useconds_t)(ms * 1000)); }
static JNIEXPORT jlong JNICALL
sc_currentUptimeMicro(JNIEnv* env, jclass c) { (void)env; (void)c; return uptime_ms() * 1000; }
static JNIEXPORT jlong JNICALL
sc_uptimeNanos(JNIEnv* env, jclass c) { (void)env; (void)c; return uptime_ms() * 1000000; }
static JNIEXPORT jlong JNICALL
sc_currentTimeMillis(JNIEnv* env, jclass c) {
    (void)env; (void)c;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static JNIEXPORT jlong JNICALL
sc_currentTimeNanos(JNIEnv* env, jclass c) {
    (void)env; (void)c;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (jlong)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static JNIEXPORT jlong JNICALL
sc_currentThreadTimeNanos(JNIEnv* env, jclass c) {
    (void)env; (void)c;
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (jlong)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* ── v11 ★ android.graphics.RenderNode（16:24 案——View 图形层）── */
static JNIEXPORT jlong JNICALL
rn_create(JNIEnv* env, jclass c, jstring name) { (void)env; (void)c; (void)name; return 1; }
static JNIEXPORT jlong JNICALL
rn_finalizer(JNIEnv* env, jclass c) { (void)env; (void)c; return 0; }
static JNIEXPORT void JNICALL
rn_void_v2(JNIEnv* env, jclass c, jlong a, jlong b) { (void)env; (void)c; (void)a; (void)b; }
static JNIEXPORT void JNICALL
rn_void_v1(JNIEnv* env, jclass c, jlong a) { (void)env; (void)c; (void)a; }
static JNIEXPORT jboolean JNICALL
rn_bool_v1(JNIEnv* env, jclass c, jlong a) { (void)env; (void)c; (void)a; return JNI_FALSE; }
static JNIEXPORT jboolean JNICALL
rn_bool_v2(JNIEnv* env, jclass c, jlong a, jboolean b) { (void)env; (void)c; (void)a; (void)b; return JNI_FALSE; }
static JNIEXPORT jint JNICALL
rn_int_v1(JNIEnv* env, jclass c, jlong a) { (void)env; (void)c; (void)a; return 0; }
static JNIEXPORT jfloat JNICALL
rn_float_v1(JNIEnv* env, jclass c, jlong a) { (void)env; (void)c; (void)a; return 0.f; }
static JNIEXPORT jlong JNICALL
rn_long_v1(JNIEnv* env, jclass c, jlong a) { (void)env; (void)c; (void)a; return 0; }
static JNIEXPORT jboolean JNICALL
rn_bool_vi(JNIEnv* env, jclass c, jlong a, jint b) { (void)env; (void)c; (void)a; (void)b; return JNI_FALSE; }
static JNIEXPORT jboolean JNICALL
rn_bool_vf(JNIEnv* env, jclass c, jlong a, jfloat b) { (void)env; (void)c; (void)a; (void)b; return JNI_FALSE; }

/* ── v8 ★ android.content.res.AssetManager（22:04 案）── */
static JNIEXPORT jlong JNICALL
am_themeFreeFunc(JNIEnv* env, jclass c) { (void)env; (void)c; return 0xdeadbeef; /* v13：非0过 libcore 检查，永不 free */ }
static JNIEXPORT jlong JNICALL
am_nativeCreate(JNIEnv* env, jclass c) { (void)env; (void)c; return 1; }
/* v9 ★：nativeGetResourceValue——AOSP 源码 L1738 签名：
 * (long ptr, int resId, short density, TypedValue outValue, boolean resolveRefs) → int cookie
 * 填 TypedValue{type=TYPE_DIMENSION(0x05), data=8(px), assetCookie=1, density=0} 返 1
 * → getDimensionPixelSize 返 8px——ViewConfiguration 全链尺寸有着落 */
static JNIEXPORT jint JNICALL
am_getResourceValue(JNIEnv* env, jclass c, jlong ptr, jint resId, jshort dens,
                    jobject outValue, jboolean resolve) {
    (void)c; (void)ptr; (void)resId; (void)dens; (void)resolve;
    if (outValue == NULL) return 0;
    jclass tv = (*env)->GetObjectClass(env, outValue);
    jfieldID fType = (*env)->GetFieldID(env, tv, "type", "I");
    jfieldID fData = (*env)->GetFieldID(env, tv, "data", "I");
    jfieldID fCookie = (*env)->GetFieldID(env, tv, "assetCookie", "I");
    jfieldID fDensity = (*env)->GetFieldID(env, tv, "density", "I");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (fType)   (*env)->SetIntField(env, outValue, fType, 0x10);   /* v10: TYPE_INT_NUMBER——getInteger 过（switch default 兜底）；dimension 若炸按 resId 再精调 */
    if (fData)   (*env)->SetIntField(env, outValue, fData, 8);      /* 8px (COMPLEX_UNIT_PX=0) */
    if (fCookie) (*env)->SetIntField(env, outValue, fCookie, 1);
    if (fDensity) (*env)->SetIntField(env, outValue, fDensity, 0);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    return 1;
}
static JNIEXPORT void JNICALL
am_setApkAssets(JNIEnv* env, jclass c, jlong ptr, jobjectArray a, jboolean b1, jboolean b2) {
    (void)env; (void)c; (void)ptr; (void)a; (void)b1; (void)b2;
}
static JNIEXPORT void JNICALL
am_nativeDestroy(JNIEnv* env, jclass c, jlong p) { (void)env; (void)c; (void)p; }
static JNIEXPORT jint JNICALL
am_addAssetPath(JNIEnv* env, jclass c, jlong p, jstring path) {
    (void)env; (void)c; (void)p; (void)path; return 0; }
static JNIEXPORT jstring JNICALL
am_getAssetPath(JNIEnv* env, jclass c, jlong p) { (void)env; (void)c; (void)p; return NULL; }

/* ── v7 ★ android.os.MessageQueue（08:14 Looper.prepare 案）── */
static JNIEXPORT jlong JNICALL
mq_nativeInit(JNIEnv* env, jclass c) {
    (void)env; (void)c; return 1;   /* 假队列句柄（非 0） */
}
static JNIEXPORT void JNICALL
mq_nativeDestroy(JNIEnv* env, jclass c, jlong ptr) { (void)env; (void)c; (void)ptr; }
static JNIEXPORT void JNICALL
mq_nativePollOnce(JNIEnv* env, jclass c, jlong ptr, jint ms, jint sp) {
    (void)env; (void)c; (void)ptr; (void)ms; (void)sp;
}
static JNIEXPORT void JNICALL
mq_nativeWake(JNIEnv* env, jclass c, jlong ptr) { (void)env; (void)c; (void)ptr; }
static JNIEXPORT jboolean JNICALL
mq_isPollLocked(JNIEnv* env, jclass c, jlong ptr) {
    (void)env; (void)c; (void)ptr; return JNI_TRUE;
}
static JNIEXPORT void JNICALL
mq_nativeSetFileDescriptorEvents(JNIEnv* env, jclass c, jlong ptr, jint fd, jint ev) {
    (void)env; (void)c; (void)ptr; (void)fd; (void)ev;
}
static JNIEXPORT void JNICALL
mq_sendMessageAtFrontOfQueue(JNIEnv* env, jclass c, jlong ptr, jobject msg) {
    (void)env; (void)c; (void)ptr; (void)msg;
}

/* ── v6 ★ android.util.EventLog（00:42 onPause 案）── */
static JNIEXPORT jint JNICALL
el_writeEvent1(JNIEnv* env, jclass c, jint tag, jfloat v) {
    (void)env; (void)c; (void)tag; (void)v; return 0;
}
static JNIEXPORT jint JNICALL
el_writeEvent2(JNIEnv* env, jclass c, jint tag, jint v) {
    (void)env; (void)c; (void)tag; (void)v; return 0;
}
static JNIEXPORT jint JNICALL
el_writeEvent3(JNIEnv* env, jclass c, jint tag, jlong v) {
    (void)env; (void)c; (void)tag; (void)v; return 0;
}
static JNIEXPORT jint JNICALL
el_writeEventArr(JNIEnv* env, jclass c, jint tag, jobjectArray v) {
    (void)env; (void)c; (void)tag; (void)v; return 0;
}
static JNIEXPORT jint JNICALL
el_writeEventStr(JNIEnv* env, jclass c, jint tag, jstring s) {
    (void)env; (void)c; (void)tag; (void)s; return 0;
}

/* ── v5 ★ android.os.BinderInternal（02:34 onStop 案）── */
static JNIEXPORT jobject JNICALL
bi_getContextObject(JNIEnv* env, jclass c) {
    (void)env; (void)c; return NULL;   /* 无系统 ServiceManager——判空方自退 */
}
static JNIEXPORT void JNICALL
bi_forceGc(JNIEnv* env, jclass c) { (void)env; (void)c; }
static JNIEXPORT jint JNICALL
bi_getContextFd(JNIEnv* env, jclass c) { (void)env; (void)c; return -1; }
static JNIEXPORT jint JNICALL
bi_handleGc(JNIEnv* env, jclass c, jlong d) { (void)env; (void)c; (void)d; return 0; }

/* ── v3 ★ android.os.Binder 相关（getCallingPid 等常被 Lifecycle 触碰）── */
static JNIEXPORT jint JNICALL
bd_getCallingPid(JNIEnv* env, jclass c) { (void)env; (void)c; return getpid(); }
/* v12：Binder 构造链（17:14 案）——native 句柄族 */
static JNIEXPORT jlong JNICALL
bd_nativeBBinderHolder(JNIEnv* env, jclass c) { (void)env; (void)c; return 1; }
static JNIEXPORT jlong JNICALL
bd_nativeGetFinalizer(JNIEnv* env, jclass c) { (void)env; (void)c; return 0; }
static JNIEXPORT jint JNICALL
bd_getCallingUid(JNIEnv* env, jclass c) { (void)env; (void)c; return getuid(); }
static JNIEXPORT jboolean JNICALL
bd_isCurrentThread(JNIEnv* env, jclass c) { (void)env; (void)c; return JNI_TRUE; }

static const JNINativeMethod kLog[] = {
    { "isLoggable",     "(Ljava/lang/String;I)Z",                     (void*)b_isLoggable },
    { "println_native", "(IILjava/lang/String;Ljava/lang/String;)I", (void*)b_println_native },
    { "logger_entry_max_payload_native", "()I",                       (void*)b_logger_max },
};
static const JNINativeMethod kSP[] = {
    { "native_find",                 "(Ljava/lang/String;)J",  (void*)sp_find },
    { "native_get",                  "(J)Ljava/lang/String;",  (void*)sp_get_handle },
    { "native_get",                  "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", (void*)sp_get },
    { "native_get_boolean",          "(JZ)Z",                  (void*)sp_get_boolean_h },
    { "native_get_boolean",          "(Ljava/lang/String;Z)Z", (void*)sp_get_boolean },
    { "native_get_int",              "(JI)I",                  (void*)sp_get_int_h },
    { "native_get_int",              "(Ljava/lang/String;I)I", (void*)sp_get_int },
    { "native_get_long",             "(JJ)J",                  (void*)sp_get_long_h },
    { "native_get_long",             "(Ljava/lang/String;J)J", (void*)sp_get_long },
    { "native_set",                  "(Ljava/lang/String;Ljava/lang/String;)V", (void*)sp_set },
    { "native_add_change_callback",  "()V",                    (void*)sp_change_cb },
    { "native_report_sysprop_change","()V",                    (void*)sp_report },
};
static const JNINativeMethod kTrace[] = {
    { "nativeIsTagEnabled",            "(J)Z",      (void*)tr_isTagEnabled },
    { "nativeTraceBegin",              "(JLjava/lang/String;)V", (void*)tr_nativeTraceBegin },
    { "nativeTraceEnd",                "(J)V",      (void*)tr_nativeTraceEnd },
    { "nativeAsyncTraceBegin",         "(JLjava/lang/String;I)V", (void*)tr_nativeAsyncTraceBegin },
};
static const JNINativeMethod kSC[] = {
    { "elapsedRealtime",              "()J",  (void*)sc_elapsedRealtime },
    { "elapsedRealtimeNanos",         "()J",  (void*)sc_elapsedRealtimeNanos },
    { "uptimeMillis",                 "()J",  (void*)sc_uptimeMillis },
    { "currentThreadTimeMillis",      "()J",  (void*)sc_currentThreadTimeMillis },
    { "currentThreadTimeMicro",       "()J",  (void*)sc_currentThreadTimeMicro },
    { "uptimeNanos",                  "()J",  (void*)sc_uptimeNanos },
};
static const JNINativeMethod kRN[] = {
    { "nCreate", "(Ljava/lang/String;)J", (void*)rn_create },
    { "nGetNativeFinalizer", "()J", (void*)rn_finalizer },
    { "nAddAnimator", "(JJ)V", (void*)rn_void_v2 },
    { "nEndAllAnimators", "(J)V", (void*)rn_void_v1 },
    { "nForceEndAnimators", "(J)V", (void*)rn_void_v1 },
    { "nDiscardDisplayList", "(J)V", (void*)rn_void_v1 },
    { "nIsValid", "(J)Z", (void*)rn_bool_v1 },
    { "nHasIdentityMatrix", "(J)Z", (void*)rn_bool_v1 },
    { "nOffsetTopAndBottom", "(JI)Z", (void*)rn_bool_vi },
    { "nOffsetLeftAndRight", "(JI)Z", (void*)rn_bool_vi },
    { "nSetLeft", "(JI)Z", (void*)rn_bool_vi },
    { "nSetTop", "(JI)Z", (void*)rn_bool_vi },
    { "nSetRight", "(JI)Z", (void*)rn_bool_vi },
    { "nSetBottom", "(JI)Z", (void*)rn_bool_vi },
    { "nSetLayerType", "(JI)Z", (void*)rn_bool_vi },
    { "nSetCameraDistance", "(JF)Z", (void*)rn_bool_vf },
    { "nSetPivotX", "(JF)Z", (void*)rn_bool_vf },
    { "nSetPivotY", "(JF)Z", (void*)rn_bool_vf },
    { "nSetAlpha", "(JF)Z", (void*)rn_bool_vf },
    { "nSetElevation", "(JF)Z", (void*)rn_bool_vf },
    { "nSetTranslationX", "(JF)Z", (void*)rn_bool_vf },
    { "nSetTranslationY", "(JF)Z", (void*)rn_bool_vf },
    { "nSetTranslationZ", "(JF)Z", (void*)rn_bool_vf },
    { "nSetRotation", "(JF)Z", (void*)rn_bool_vf },
    { "nSetRotationX", "(JF)Z", (void*)rn_bool_vf },
    { "nSetRotationY", "(JF)Z", (void*)rn_bool_vf },
    { "nSetScaleX", "(JF)Z", (void*)rn_bool_vf },
    { "nSetScaleY", "(JF)Z", (void*)rn_bool_vf },
    { "nSetHasOverlappingRendering", "(JZ)Z", (void*)rn_bool_v2 },
    { "nSetClipToBounds", "(JZ)Z", (void*)rn_bool_v2 },
    { "nGetClipToBounds", "(J)Z", (void*)rn_bool_v1 },
    { "nGetLeft", "(J)I", (void*)rn_int_v1 },
    { "nGetTop", "(J)I", (void*)rn_int_v1 },
    { "nGetRight", "(J)I", (void*)rn_int_v1 },
    { "nGetBottom", "(J)I", (void*)rn_int_v1 },
    { "nGetLayerType", "(J)I", (void*)rn_int_v1 },
    { "nGetWidth", "(J)I", (void*)rn_int_v1 },
    { "nGetHeight", "(J)I", (void*)rn_int_v1 },
    { "nGetUsageSize", "(J)I", (void*)rn_int_v1 },
    { "nGetAllocatedSize", "(J)I", (void*)rn_int_v1 },
    { "nGetAlpha", "(J)F", (void*)rn_float_v1 },
    { "nGetScaleX", "(J)F", (void*)rn_float_v1 },
    { "nGetScaleY", "(J)F", (void*)rn_float_v1 },
    { "nGetElevation", "(J)F", (void*)rn_float_v1 },
    { "nGetTranslationX", "(J)F", (void*)rn_float_v1 },
    { "nGetTranslationY", "(J)F", (void*)rn_float_v1 },
    { "nGetTranslationZ", "(J)F", (void*)rn_float_v1 },
    { "nGetRotation", "(J)F", (void*)rn_float_v1 },
    { "nGetRotationX", "(J)F", (void*)rn_float_v1 },
    { "nGetRotationY", "(J)F", (void*)rn_float_v1 },
    { "nGetPivotX", "(J)F", (void*)rn_float_v1 },
    { "nGetPivotY", "(J)F", (void*)rn_float_v1 },
    { "nGetCameraDistance", "(J)F", (void*)rn_float_v1 },
    { "nHasOverlappingRendering", "(J)Z", (void*)rn_bool_v1 },
    { "nGetClipToOutline", "(J)Z", (void*)rn_bool_v1 },
    { "nIsPivotExplicitlySet", "(J)Z", (void*)rn_bool_v1 },
    { "nGetAllowForceDark", "(J)Z", (void*)rn_bool_v1 },
    { "nSetAllowForceDark", "(JZ)Z", (void*)rn_bool_v2 },
    { "nGetUniqueId", "(J)J", (void*)rn_long_v1 },
    { "nOutput", "(J)V", (void*)rn_void_v1 },
    { "nResetPivot", "(J)V", (void*)rn_void_v1 },
    { "nSetUsageHint", "(JI)V", (void*)rn_void_v2 },
    { "nSetIsTextureView", "(J)V", (void*)rn_void_v1 },
};
/* ── v13 ★★ Theme/Style 族（20:48 TA 指令：找资料一次性实现阶段功能）
 * Robolectric Shadow 思路（robolectric.org/javadoc ShadowResources/ShadowAssetManager）：
 * native 返安全值 + Java 侧字段 Unsafe 填真壳——整条
 * ContextThemeWrapper.getTheme:210 → Resources.newTheme[final] →
 * ResourcesImpl.newThemeImpl:1412 → new ThemeImpl():1441 →
 * mAssets.createTheme() → nativeThemeCreate 链打通。
 * 签名全部照抄 AOSP 36 AssetManager.java:1781-1795 逐字核对 */
static JNIEXPORT jlong JNICALL am_themeCreate(JNIEnv* e, jclass c) { (void)e; (void)c; return 1; }
static JNIEXPORT void JNICALL am_themeApplyStyle(JNIEnv* e, jclass c) { (void)e; (void)c; }
static JNIEXPORT void JNICALL am_themeRebase(JNIEnv* e, jclass c) { (void)e; (void)c; }
static JNIEXPORT void JNICALL am_themeCopy(JNIEnv* e, jclass c) { (void)e; (void)c; }
static JNIEXPORT jint JNICALL am_themeGetAttrValue(JNIEnv* e, jclass c, jlong a, jlong b, jint d, jobject v, jboolean f) { (void)e; (void)c; (void)a; (void)b; (void)d; (void)v; (void)f; return 0; }
static JNIEXPORT jint JNICALL am_themeGetChangingCfg(JNIEnv* e, jclass c, jlong t) { (void)e; (void)c; (void)t; return 0; }
static JNIEXPORT void JNICALL am_themeDump(JNIEnv* e, jclass c) { (void)e; (void)c; }
static JNIEXPORT jint JNICALL am_themeGetParentThemeId(JNIEnv* e, jclass c, jlong p, jint s) { (void)e; (void)c; (void)p; (void)s; return 0; }
static JNIEXPORT void JNICALL am_applyStyle(JNIEnv* e, jclass c) { (void)e; (void)c; }
static JNIEXPORT jboolean JNICALL am_resolveAttrs(JNIEnv* e, jclass c) { (void)e; (void)c; return 0; }
static JNIEXPORT jboolean JNICALL am_retrieveAttributes(JNIEnv* e, jclass c) { (void)e; (void)c; return 0; }
static JNIEXPORT jint JNICALL am_getResourceIdentifier(JNIEnv* e, jclass c, jlong p, jstring n, jstring t, jstring k) { (void)e; (void)c; (void)p; (void)n; (void)t; (void)k; return 0; }
static JNIEXPORT jintArray JNICALL am_getStyleAttributes(JNIEnv* e, jclass c, jlong p, jint s) { (void)e; (void)c; (void)p; (void)s; return NULL; }

/* v14b：kParcel 注册撤销（10:22 案：JNI_OnLoad 里 FindClass(Parcel) 触发
 * ColorOS 二级 dex 类链接 → <clinit> 链 SEGV。kParcel 实现已物理移除；
 * v89 主修（ATM 单例预填）已绕开 Parcel 路径。 */


/* ── v17 ★★ 族级通用桩（descriptor 由 ROM framework.dex 实测生成）
 * 返回类型分族：void/false/int/1/0f/0.0/null；finalizer 返回【真实 no-op 函数地址】
 * （旧实现返回 0xdeadbeef/1 —— NativeAllocationRegistry 真回调时会跳飞）。 */
static void tvm_noop_free(void* p) { (void)p; }
static jlong tvm_finalizer_addr(void) { return (jlong)(intptr_t)&tvm_noop_free; }
static JNIEXPORT void    JNICALL tvm_stub_void(JNIEnv* e, jclass c, ...) { (void)e; (void)c; }
static JNIEXPORT jboolean JNICALL tvm_stub_false(JNIEnv* e, jclass c, ...) { (void)e; (void)c; return JNI_FALSE; }
static JNIEXPORT jint    JNICALL tvm_stub_int(JNIEnv* e, jclass c, ...) { (void)e; (void)c; return 0; }
static JNIEXPORT jlong   JNICALL tvm_stub_one(JNIEnv* e, jclass c, ...) { (void)e; (void)c; return 1; }
static JNIEXPORT jfloat  JNICALL tvm_stub_float(JNIEnv* e, jclass c, ...) { (void)e; (void)c; return 0.f; }
static JNIEXPORT jdouble JNICALL tvm_stub_double(JNIEnv* e, jclass c, ...) { (void)e; (void)c; return 0.0; }
static JNIEXPORT jobject JNICALL tvm_stub_null(JNIEnv* e, jclass c, ...) { (void)e; (void)c; return NULL; }
static JNIEXPORT jlong   JNICALL tvm_stub_finalizer(JNIEnv* e, jclass c, ...) { (void)e; (void)c; return tvm_finalizer_addr(); }

#include "gen_registry.h"
#include "gen_missing.h"

/* ── v17 ★★ ColorSpace 族（ROM framework.dex 实测：android/graphics/ColorSpace$Rgb$Native）
 * 案：ColorSpace.<clinit>:1603 → new Rgb(SRGB_TRANSFER_PARAMETERS)
 *   → Rgb.<init>:3154 mNativePtr = Native.nativeCreate(7F,[F)
 *   → 同时 NoImagePreloadHolder.sRegistry 构造调 nativeGetNativeFinalizer()。
 * 语义：SkColorSpace* 只作不透明句柄（后续 Skia 全桩，不解引用）；
 *   finalizer 返回非 0 伪函数指针（NativeAllocationRegistry 要求非 0，
 *   永不释放——同 v13 am_themeFreeFunc 策略）。 */
static JNIEXPORT jlong JNICALL
cs_nativeFinalizer(JNIEnv* e, jclass c) {
    (void)e; (void)c; return tvm_finalizer_addr();
}
static JNIEXPORT jlong JNICALL
cs_nativeCreate(JNIEnv* e, jclass c, jfloat a, jfloat b, jfloat cc, jfloat d,
                jfloat f, jfloat g, jfloat h, jfloatArray xyz) {
    (void)e; (void)c; (void)a; (void)b; (void)cc; (void)d; (void)f; (void)g;
    (void)h; (void)xyz;
    return 1;   /* 稳定非 0 句柄 */
}
static const JNINativeMethod kColorSpace[] = {
    { "nativeGetNativeFinalizer", "()J",         (void*)cs_nativeFinalizer },
    { "nativeCreate",             "(FFFFFFF[F)J", (void*)cs_nativeCreate },
};

/* ── v15 ★★ Paint/Skia 族（21:57 案：TextPaint.<init>:55 → Paint.<init>:706
 * → nInit UnsatisfiedLinkError）。签名逐行照抄 AOSP 36 Paint.java:3782-3958。
 * 语义设计（对齐 Robolectric ShadowPaint）：
 *   nInit/nInitWithPaint/nGetNativeFinalizer → 1（稳定指针）
 *   读族 int/float/boolean → 0/0f/false；写族 void → 空；
 *   nBreakText → 0；nHasGlyph → false；路径/边界 void。
 * Paint.<clinit> 无 native（sRegistry 在 NoImagePreloadHolder 嵌套类惰性——
 * AOSP :77 亲自核实）——FindClass(Paint) 链接安全（非 10:22 Parcel 案）。 */
static JNIEXPORT jlong JNICALL pn_long_one0(JNIEnv* e, jclass c) { (void)e; (void)c; return 1; }
static JNIEXPORT jlong JNICALL pn_long_one1(JNIEnv* e, jclass c, jlong a) { (void)e; (void)c; (void)a; return 1; }
static JNIEXPORT void JNICALL pn_void0(JNIEnv* e, jclass c) { (void)e; (void)c; }
static JNIEXPORT void JNICALL pn_void_ptr(JNIEnv* e, jclass c, jlong p) { (void)e; (void)c; (void)p; }
static JNIEXPORT void JNICALL pn_void_ptr_i(JNIEnv* e, jclass c, jlong p, jint a) { (void)e; (void)c; (void)p; (void)a; }
static JNIEXPORT void JNICALL pn_void_ptr_f(JNIEnv* e, jclass c, jlong p, jfloat a) { (void)e; (void)c; (void)p; (void)a; }
static JNIEXPORT void JNICALL pn_void_ptr_z(JNIEnv* e, jclass c, jlong p, jboolean a) { (void)e; (void)c; (void)p; (void)a; }
static JNIEXPORT void JNICALL pn_void_ptr_j(JNIEnv* e, jclass c, jlong p, jlong a) { (void)e; (void)c; (void)p; (void)a; }
static JNIEXPORT jint JNICALL pn_int0(JNIEnv* e, jclass c, jlong p) { (void)e; (void)c; (void)p; return 0; }
static JNIEXPORT jfloat JNICALL pn_float0(JNIEnv* e, jclass c, jlong p) { (void)e; (void)c; (void)p; return 0.f; }
static JNIEXPORT jboolean JNICALL pn_bool0(JNIEnv* e, jclass c, jlong p) { (void)e; (void)c; (void)p; return 0; }
/* 复合签名族（breakText/advances/metrics——AOSP:3785-3836 原文） */
static JNIEXPORT jint JNICALL pn_breakTextC(JNIEnv* e, jclass c, jlong p, jcharArray t, jint i, jint n, jfloat w, jint b, jfloatArray mw) { (void)e;(void)c;(void)p;(void)t;(void)i;(void)n;(void)w;(void)b;(void)mw; return 0; }
static JNIEXPORT jint JNICALL pn_breakTextS(JNIEnv* e, jclass c, jlong p, jstring t, jboolean mf, jfloat w, jint b, jfloatArray mw) { (void)e;(void)c;(void)p;(void)t;(void)mf;(void)w;(void)b;(void)mw; return 0; }
static JNIEXPORT jfloat JNICALL pn_advancesC(JNIEnv* e, jclass c, jlong p, jcharArray t, jint i, jint n, jint ai, jint bi, jfloatArray adv) { (void)e;(void)c;(void)p;(void)t;(void)i;(void)n;(void)ai;(void)bi;(void)adv; return 0.f; }
static JNIEXPORT jfloat JNICALL pn_advancesS(JNIEnv* e, jclass c, jlong p, jstring t, jint s, jint en, jint ai, jint bi, jfloatArray adv) { (void)e;(void)c;(void)p;(void)t;(void)s;(void)en;(void)ai;(void)bi;(void)adv; return 0.f; }
static JNIEXPORT jfloat JNICALL pn_runAdvance(JNIEnv* e, jclass c, jlong p, jcharArray t, jint s, jint en, jint cs, jint ce, jint bi, jboolean rtl) { (void)e;(void)c;(void)p;(void)t;(void)s;(void)en;(void)cs;(void)ce;(void)bi;(void)rtl; return 0.f; }
static JNIEXPORT jfloat JNICALL pn_runCharAdv(JNIEnv* e, jclass c, jlong p, jcharArray t, jint s, jint en, jint cs, jint ce, jint bi) { (void)e;(void)c;(void)p;(void)t;(void)s;(void)en;(void)cs;(void)ce;(void)bi; return 0.f; }
static JNIEXPORT jint JNICALL pn_offsetForAdvance(JNIEnv* e, jclass c, jlong p, jcharArray t, jint s, jint en, jint cs, jint ce, jint bi, jboolean rtl, jfloat a) { (void)e;(void)c;(void)p;(void)t;(void)s;(void)en;(void)cs;(void)ce;(void)bi;(void)rtl;(void)a; return 0; }
static JNIEXPORT void JNICALL pn_metricsF(JNIEnv* e, jclass c, jlong p, jobject m, jboolean a) { (void)e;(void)c;(void)p;(void)m;(void)a; }
static JNIEXPORT jint JNICALL pn_metricsI(JNIEnv* e, jclass c, jlong p, jobject m, jboolean a) { (void)e;(void)c;(void)p;(void)m;(void)a; return 0; }
static JNIEXPORT void JNICALL pn_metricsForTextC(JNIEnv* e, jclass c, jlong p, jcharArray t, jint i, jint n, jint bi, jobject m) { (void)e;(void)c;(void)p;(void)t;(void)i;(void)n;(void)bi;(void)m; }
static JNIEXPORT void JNICALL pn_metricsForTextS(JNIEnv* e, jclass c, jlong p, jstring t, jint s, jint en, jint bi, jobject m) { (void)e;(void)c;(void)p;(void)t;(void)s;(void)en;(void)bi;(void)m; }
static JNIEXPORT void JNICALL pn_textPathC(JNIEnv* e, jclass c, jlong p, jint bf, jcharArray t, jint i, jint n, jfloat x, jfloat y, jlong path) { (void)e;(void)c;(void)p;(void)bf;(void)t;(void)i;(void)n;(void)x;(void)y;(void)path; }
static JNIEXPORT void JNICALL pn_textPathS(JNIEnv* e, jclass c, jlong p, jint bf, jstring t, jint s, jint en, jfloat x, jfloat y, jlong path) { (void)e;(void)c;(void)p;(void)bf;(void)t;(void)s;(void)en;(void)x;(void)y;(void)path; }
static JNIEXPORT void JNICALL pn_strBounds(JNIEnv* e, jclass c, jlong p, jstring t, jint s, jint en, jint bi, jobject r) { (void)e;(void)c;(void)p;(void)t;(void)s;(void)en;(void)bi;(void)r; }
static JNIEXPORT void JNICALL pn_charBounds(JNIEnv* e, jclass c, jlong p, jcharArray t, jint i, jint n, jint bi, jobject r) { (void)e;(void)c;(void)p;(void)t;(void)i;(void)n;(void)bi;(void)r; }
static JNIEXPORT jboolean JNICALL pn_hasGlyph(JNIEnv* e, jclass c, jlong p, jint bf, jstring s2) { (void)e;(void)c;(void)p;(void)bf;(void)s2; return 0; }
static JNIEXPORT jint JNICALL pn_setTextLocales(JNIEnv* e, jclass c, jlong p, jstring l) { (void)e;(void)c;(void)p;(void)l; return 0; }
static JNIEXPORT void JNICALL pn_fontFeat(JNIEnv* e, jclass c, jlong p, jstring s3) { (void)e;(void)c;(void)p;(void)s3; }
static JNIEXPORT void JNICALL pn_shadow(JNIEnv* e, jclass c, jlong p, jfloat r, jfloat dx, jfloat dy, jlong col) { (void)e;(void)c;(void)p;(void)r;(void)dx;(void)dy;(void)col; }
static JNIEXPORT void JNICALL pn_setColorCS(JNIEnv* e, jclass c, jlong p, jlong cs, jfloat r, jfloat g, jfloat b, jlong a) { (void)e;(void)c;(void)p;(void)cs;(void)r;(void)g;(void)b;(void)a; }
static JNIEXPORT void JNICALL pn_setColorI(JNIEnv* e, jclass c, jlong p, jint col) { (void)e;(void)c;(void)p;(void)col; }
static JNIEXPORT jlong JNICALL pn_setShader(JNIEnv* e, jclass c, jlong p, jlong sh) { (void)e;(void)c;(void)p;(void)sh; return 0; }
static JNIEXPORT jint JNICALL pn_getElegant(JNIEnv* e, jclass c, jlong p) { (void)e;(void)c;(void)p; return 0; }
static JNIEXPORT void JNICALL pn_setElegant(JNIEnv* e, jclass c, jlong p, jint v) { (void)e;(void)c;(void)p;(void)v; }

static const JNINativeMethod kPaint[] = {
    { "nGetNativeFinalizer", "()J", (void*)pn_long_one0 },
    { "nInit", "()J", (void*)pn_long_one0 },
    { "nInitWithPaint", "(J)J", (void*)pn_long_one1 },
    { "nBreakText", "(J[C IIF I[F)I", (void*)pn_breakTextC },
    { "nBreakText", "(JLjava/lang/String;ZF I[F)I", (void*)pn_breakTextS },
    { "nGetTextAdvances", "(J[C IIII[F)F", (void*)pn_advancesC },
    { "nGetTextAdvances", "(JLjava/lang/String;IIII[F)F", (void*)pn_advancesS },
    { "nGetTextPath", "(JI[CIIFJ)V", (void*)pn_textPathC },
    { "nGetTextPath", "(JILjava/lang/String;IIFFJ)V", (void*)pn_textPathS },
    { "nGetStringBounds", "(JLjava/lang/String;III Landroid/graphics/Rect;)V", (void*)pn_strBounds },
    { "nGetCharArrayBounds", "(J[CIII Landroid/graphics/Rect;)V", (void*)pn_charBounds },
    { "nHasGlyph", "(JILjava/lang/String;)Z", (void*)pn_hasGlyph },
    { "nGetRunAdvance", "(J[CIIIIIZ)F", (void*)pn_runAdvance },
    { "nGetRunCharacterAdvance", "(J[CIIIII)F", (void*)pn_runCharAdv },
    { "nGetOffsetForAdvance", "(J[CIIIIIZF)I", (void*)pn_offsetForAdvance },
    { "nGetFontMetricsIntForText", "(J[CIII Landroid/graphics/FontMetricsInt;)V", (void*)pn_metricsForTextC },
    { "nGetFontMetricsIntForText", "(JLjava/lang/String;III Landroid/graphics/FontMetricsInt;)V", (void*)pn_metricsForTextS },
    { "nSetTextLocales", "(JLjava/lang/String;)I", (void*)pn_setTextLocales },
    { "nSetFontFeatureSettings", "(JLjava/lang/String;)V", (void*)pn_fontFeat },
    { "nGetFontMetrics", "(J Landroid/graphics/Paint$FontMetrics; Z)F", (void*)pn_metricsF },
    { "nGetFontMetricsInt", "(J Landroid/graphics/Paint$FontMetricsInt; Z)I", (void*)pn_metricsI },
    { "nReset", "(J)V", (void*)pn_void_ptr },
    { "nSet", "(JJ)V", (void*)pn_void_ptr_j },
    { "nGetStyle", "(J)I", (void*)pn_int0 },
    { "nSetStyle", "(JI)V", (void*)pn_void_ptr_i },
    { "nGetStrokeCap", "(J)I", (void*)pn_int0 },
    { "nSetStrokeCap", "(JI)V", (void*)pn_void_ptr_i },
    { "nGetStrokeJoin", "(J)I", (void*)pn_int0 },
    { "nSetStrokeJoin", "(JI)V", (void*)pn_void_ptr_i },
    { "nGetFillPath", "(JJJ)Z", (void*)pn_bool0 },
    { "nSetShader", "(JJ)J", (void*)pn_setShader },
    { "nSetColorFilter", "(JJ)J", (void*)pn_setShader },
    { "nSetXfermode", "(JI)V", (void*)pn_void_ptr_i },
    { "nSetXfermode", "(JJ)V", (void*)pn_void_ptr_j },
    { "nSetPathEffect", "(JJ)J", (void*)pn_setShader },
    { "nSetMaskFilter", "(JJ)J", (void*)pn_setShader },
    { "nSetTypeface", "(JJ)V", (void*)pn_void_ptr_j },
    { "nGetTextAlign", "(J)I", (void*)pn_int0 },
    { "nSetTextAlign", "(JI)V", (void*)pn_void_ptr_i },
    { "nSetTextLocalesByMinikinLocaleListId", "(JI)V", (void*)pn_void_ptr_i },
    { "nSetShadowLayer", "(JFFFFJ)V", (void*)pn_shadow },
    { "nHasShadowLayer", "(J)Z", (void*)pn_bool0 },
    { "nGetLetterSpacing", "(J)F", (void*)pn_float0 },
    { "nSetLetterSpacing", "(JF)V", (void*)pn_void_ptr_f },
    { "nGetWordSpacing", "(J)F", (void*)pn_float0 },
    { "nSetWordSpacing", "(JF)V", (void*)pn_void_ptr_f },
    { "nGetStartHyphenEdit", "(J)I", (void*)pn_int0 },
    { "nGetEndHyphenEdit", "(J)I", (void*)pn_int0 },
    { "nSetStartHyphenEdit", "(JI)V", (void*)pn_void_ptr_i },
    { "nSetEndHyphenEdit", "(JI)V", (void*)pn_void_ptr_i },
    { "nSetStrokeMiter", "(JF)V", (void*)pn_void_ptr_f },
    { "nGetStrokeMiter", "(J)F", (void*)pn_float0 },
    { "nSetStrokeWidth", "(JF)V", (void*)pn_void_ptr_f },
    { "nGetStrokeWidth", "(J)F", (void*)pn_float0 },
    { "nSetAlpha", "(JI)V", (void*)pn_void_ptr_i },
    { "nSetDither", "(JZ)V", (void*)pn_void_ptr_z },
    { "nGetFlags", "(J)I", (void*)pn_int0 },
    { "nSetFlags", "(JI)V", (void*)pn_void_ptr_i },
    { "nGetHinting", "(J)I", (void*)pn_int0 },
    { "nSetHinting", "(JI)V", (void*)pn_void_ptr_i },
    { "nSetAntiAlias", "(JZ)V", (void*)pn_void_ptr_z },
    { "nSetLinearText", "(JZ)V", (void*)pn_void_ptr_z },
    { "nSetSubpixelText", "(JZ)V", (void*)pn_void_ptr_z },
    { "nSetUnderlineText", "(JZ)V", (void*)pn_void_ptr_z },
    { "nSetFakeBoldText", "(JZ)V", (void*)pn_void_ptr_z },
    { "nSetFilterBitmap", "(JZ)V", (void*)pn_void_ptr_z },
    { "nSetColor", "(JJFF FJ)V", (void*)pn_setColorCS },
    { "nSetColor", "(JI)V", (void*)pn_setColorI },
    { "nSetStrikeThruText", "(JZ)V", (void*)pn_void_ptr_z },
    { "nGetElegantTextHeight", "(J)I", (void*)pn_getElegant },
    { "nSetElegantTextHeight", "(JI)V", (void*)pn_setElegant },
    { "nGetTextSize", "(J)F", (void*)pn_float0 },
    { "nGetTextScaleX", "(J)F", (void*)pn_float0 },
    { "nSetTextScaleX", "(JF)V", (void*)pn_void_ptr_f },
    { "nGetTextSkewX", "(J)F", (void*)pn_float0 },
    { "nSetTextSkewX", "(JF)V", (void*)pn_void_ptr_f },
    { "nAscent", "(J)F", (void*)pn_float0 },
    { "nDescent", "(J)F", (void*)pn_float0 },
    { "nGetUnderlinePosition", "(J)F", (void*)pn_float0 },
    { "nGetUnderlineThickness", "(J)F", (void*)pn_float0 },
};

/* ── v16 ★★（22:01 TA 指令：多步推演后一次做完）
 * Typeface 17 个（AOSP Typeface.java:1678-1719 逐行照抄）：
 * <clinit> 的 nativeWarmUpCache、defaultFromStyle 的
 * nativeCreateFromTypeface 族、nativeGetStyle/Weight、
 * nativeSetDefault、nativeGetReleaseFunc（NativeAllocationRegistry 用） */
static JNIEXPORT jlong JNICALL tf_long1(JNIEnv* e, jclass c, jlong a) { (void)e;(void)c;(void)a; return 1; }
static JNIEXPORT jlong JNICALL tf_long2(JNIEnv* e, jclass c, jlong a, jint b) { (void)e;(void)c;(void)a;(void)b; return 1; }
static JNIEXPORT jlong JNICALL tf_long3(JNIEnv* e, jclass c, jlong a, jint b, jboolean d) { (void)e;(void)c;(void)a;(void)b;(void)d; return 1; }
static JNIEXPORT jlong JNICALL tf_long4(JNIEnv* e, jclass c, jlong a, jobject l) { (void)e;(void)c;(void)a;(void)l; return 1; }
static JNIEXPORT jlong JNICALL tf_long_arr(JNIEnv* e, jclass c, jlongArray a, jlong b, jint d, jint f) { (void)e;(void)c;(void)a;(void)b;(void)d;(void)f; return 1; }
static JNIEXPORT jintArray JNICALL tf_axes(JNIEnv* e, jclass c, jlong a) { (void)e;(void)c;(void)a; return NULL; }
static JNIEXPORT void JNICALL tf_setDefault(JNIEnv* e, jclass c, jlong a) { (void)e;(void)c;(void)a; }
static JNIEXPORT jint JNICALL tf_style(JNIEnv* e, jclass c, jlong a) { (void)e;(void)c;(void)a; return 0; }
static JNIEXPORT jint JNICALL tf_weight(JNIEnv* e, jclass c, jlong a) { (void)e;(void)c;(void)a; return 400; }
static JNIEXPORT jlong JNICALL tf_releaseFunc(JNIEnv* e, jclass c) { (void)e;(void)c; return 0xdeadbeef; }
static JNIEXPORT void JNICALL tf_regFamily(JNIEnv* e, jclass c, jstring s, jlong a) { (void)e;(void)c;(void)s;(void)a; }
static JNIEXPORT jint JNICALL tf_write(JNIEnv* e, jclass c, jobject b, jint p, jlongArray n) { (void)e;(void)c;(void)b;(void)p;(void)n; return 0; }
static JNIEXPORT jlongArray JNICALL tf_read(JNIEnv* e, jclass c, jobject b, jint p) { (void)e;(void)c;(void)b;(void)p; return NULL; }
static JNIEXPORT void JNICALL tf_forceSet(JNIEnv* e, jclass c, jstring f, jobject t) { (void)e;(void)c;(void)f;(void)t; }
static JNIEXPORT void JNICALL tf_addFontColl(JNIEnv* e, jclass c, jlong a) { (void)e;(void)c;(void)a; }
static JNIEXPORT void JNICALL tf_warmUp(JNIEnv* e, jclass c, jstring f) { (void)e;(void)c;(void)f; }
static JNIEXPORT void JNICALL tf_regLocale(JNIEnv* e, jclass c, jstring l) { (void)e;(void)c;(void)l; }

static const JNINativeMethod kTypeface[] = {
    { "nativeCreateFromTypeface",             "(JI)J",              (void*)tf_long2 },
    { "nativeCreateFromTypefaceWithExactStyle", "(JIZ)J",           (void*)tf_long3 },
    { "nativeCreateFromTypefaceWithVariation", "(JLjava/util/List;)J", (void*)tf_long4 },
    { "nativeCreateWeightAlias",              "(JI)J",              (void*)tf_long2 },
    { "nativeCreateFromArray",                "([JJII)J",           (void*)tf_long_arr },
    { "nativeGetSupportedAxes",               "(J)[I",              (void*)tf_axes },
    { "nativeSetDefault",                     "(J)V",               (void*)tf_setDefault },
    { "nativeGetStyle",                       "(J)I",               (void*)tf_style },
    { "nativeGetWeight",                      "(J)I",               (void*)tf_weight },
    { "nativeGetReleaseFunc",                 "()J",                (void*)tf_releaseFunc },
    { "nativeRegisterGenericFamily",           "(Ljava/lang/String;J)V", (void*)tf_regFamily },
    { "nativeWriteTypefaces",                 "(Ljava/nio/ByteBuffer;I[J)I", (void*)tf_write },
    { "nativeReadTypefaces",                   "(Ljava/nio/ByteBuffer;I)[J", (void*)tf_read },
    { "nativeForceSetStaticFinalField",        "(Ljava/lang/String;Landroid/graphics/Typeface;)V", (void*)tf_forceSet },
    { "nativeAddFontCollections",             "(J)V",               (void*)tf_addFontColl },
    { "nativeWarmUpCache",                    "(Ljava/lang/String;)V", (void*)tf_warmUp },
    { "nativeRegisterLocaleList",             "(Ljava/lang/String;)V", (void*)tf_regLocale },
};

/* ── v16 ★★ AM 缺口 34 个（AssetManager.java:1709-1857 逐行照抄）：
 * 文本/值读取族（obtainStyledAttributes/TypedArray 必经）+
 * XmlAsset 族（LayoutInflater inflate XML 必经）+ Asset 流族 */
static JNIEXPORT void JNICALL am2_void0(JNIEnv* e, jclass c) { (void)e;(void)c; }
static JNIEXPORT void JNICALL am2_void_j(JNIEnv* e, jclass c, jlong a) { (void)e;(void)c;(void)a; }
static JNIEXPORT void JNICALL am2_void_jz(JNIEnv* e, jclass c, jlong a, jboolean b) { (void)e;(void)c;(void)a;(void)b; }
static JNIEXPORT jint JNICALL am2_int_j(JNIEnv* e, jclass c, jlong a) { (void)e;(void)c;(void)a; return 0; }
static JNIEXPORT jboolean JNICALL am2_bool_j(JNIEnv* e, jclass c, jlong a) { (void)e;(void)c;(void)a; return 0; }
static JNIEXPORT jlong JNICALL am2_long_j(JNIEnv* e, jclass c, jlong a) { (void)e;(void)c;(void)a; return 1; }
static JNIEXPORT jint JNICALL am2_int_ji2(JNIEnv* e, jclass c, jlong a, jint b) { (void)e;(void)c;(void)a;(void)b; return 0; }
static JNIEXPORT jint JNICALL am2_int_ji3(JNIEnv* e, jclass c, jlong a, jint b, jint d) { (void)e;(void)c;(void)a;(void)b;(void)d; return 0; }
static JNIEXPORT jstring JNICALL am2_str_j(JNIEnv* e, jclass c, jlong a) { (void)e;(void)c;(void)a; return NULL; }
static JNIEXPORT jstring JNICALL am2_str_ji(JNIEnv* e, jclass c, jlong a, jint b) { (void)e;(void)c;(void)a;(void)b; return NULL; }
static JNIEXPORT jint JNICALL am2_bagValue(JNIEnv* e, jclass c, jlong a, jint b, jint d, jobject v, jboolean f) { (void)e;(void)c;(void)a;(void)b;(void)d;(void)v;(void)f; return 0; }
static JNIEXPORT jobjectArray JNICALL am2_strArr(JNIEnv* e, jclass c, jlong a, jint b) { (void)e;(void)c;(void)a;(void)b; return NULL; }
static JNIEXPORT jintArray JNICALL am2_intArr(JNIEnv* e, jclass c, jlong a, jint b) { (void)e;(void)c;(void)a;(void)b; return NULL; }
static JNIEXPORT jint JNICALL am2_arrSize(JNIEnv* e, jclass c, jlong a, jint b) { (void)e;(void)c;(void)a;(void)b; return 0; }
static JNIEXPORT jint JNICALL am2_getArr(JNIEnv* e, jclass c, jlong a, jint b, jintArray v) { (void)e;(void)c;(void)a;(void)b;(void)v; return 0; }
static JNIEXPORT jobjectArray JNICALL am2_strArr_js(JNIEnv* e, jclass c, jlong a, jstring b) { (void)e;(void)c;(void)a;(void)b; return NULL; }
static JNIEXPORT jobjectArray JNICALL am2_getLocales(JNIEnv* e, jclass c, jlong a, jboolean b) { (void)e;(void)c;(void)a;(void)b; return NULL; }
static JNIEXPORT jobjectArray JNICALL am2_sizeCfgs(JNIEnv* e, jclass c, jlong a) { (void)e;(void)c;(void)a; return NULL; }
static JNIEXPORT jobjectArray JNICALL am2_sizeUiCfgs(JNIEnv* e, jclass c, jlong a, jboolean b) { (void)e;(void)c;(void)a;(void)b; return NULL; }
static JNIEXPORT jobject JNICALL am2_map(JNIEnv* e, jclass c, jlong a, jstring b) { (void)e;(void)c;(void)a;(void)b; return NULL; }
static JNIEXPORT jstring JNICALL am2_lastRes(JNIEnv* e, jclass c, jlong a) { (void)e;(void)c;(void)a; return NULL; }
static JNIEXPORT jintArray JNICALL am2_attrStack(JNIEnv* e, jclass c, jlong a, jlong b, jint d, jint f) { (void)e;(void)c;(void)a;(void)b;(void)d;(void)f; return NULL; }
static JNIEXPORT void JNICALL am2_setCfg(JNIEnv* e, jclass c) { (void)e; (void)c; /* 21参桩——JNI按描述符打包，C侧全收为可变参——足够（注册表签名匹配才是关键） */ }
static JNIEXPORT jobject JNICALL am2_pfd(JNIEnv* e, jclass c, jlong a, jstring b, jlongArray o) { (void)e;(void)c;(void)a;(void)b;(void)o; return NULL; }
static JNIEXPORT jobject JNICALL am2_pfd2(JNIEnv* e, jclass c, jlong a, jint b, jstring d, jlongArray o) { (void)e;(void)c;(void)a;(void)b;(void)d;(void)o; return NULL; }
static JNIEXPORT jint JNICALL am2_assetRead(JNIEnv* e, jclass c, jlong a, jbyteArray b, jint o, jint l) { (void)e;(void)c;(void)a;(void)b;(void)o;(void)l; return -1; }

static const JNINativeMethod kAM2[] = {
    { "nativeContainsAllocatedTable",   "(J)Z",                       (void*)am2_bool_j },
    { "nativeList",                      "(JLjava/lang/String;)[Ljava/lang/String;", (void*)am2_strArr_js },
    { "nativeOpenAsset",                "(JLjava/lang/String;I)J",    (void*)am2_long_j },
    { "nativeOpenAssetFd",              "(JLjava/lang/String;[J)Landroid/os/ParcelFileDescriptor;", (void*)am2_pfd },
    { "nativeOpenNonAsset",             "(JILjava/lang/String;I)J",    (void*)am2_long_j },
    { "nativeOpenNonAssetFd",           "(JILjava/lang/String;[J)Landroid/os/ParcelFileDescriptor;", (void*)am2_pfd2 },
    { "nativeOpenXmlAsset",             "(JILjava/lang/String;)J",    (void*)am2_long_j },
    { "nativeGetResourceStringArray",  "(JI)[Ljava/lang/String;",    (void*)am2_strArr },
    { "nativeGetResourceStringArrayInfo", "(JI)[I",                    (void*)am2_intArr },
    { "nativeGetResourceIntArray",      "(JI)[I",                     (void*)am2_intArr },
    { "nativeGetResourceArraySize",     "(JI)I",                      (void*)am2_arrSize },
    { "nativeGetResourceArray",         "(JI[I)I",                    (void*)am2_getArr },
    { "nativeGetResourceName",          "(JI)Ljava/lang/String;",     (void*)am2_str_ji },
    { "nativeGetResourcePackageName",   "(JI)Ljava/lang/String;",     (void*)am2_str_ji },
    { "nativeGetResourceTypeName",      "(JI)Ljava/lang/String;",     (void*)am2_str_ji },
    { "nativeGetResourceEntryName",     "(JI)Ljava/lang/String;",     (void*)am2_str_ji },
    { "nativeGetLocales",              "(JZ)[Ljava/lang/String;",    (void*)am2_getLocales },
    { "nativeGetSizeConfigurations",   "(J)[Landroid/content/res/Configuration;", (void*)am2_sizeCfgs },
    { "nativeSetResourceResolutionLoggingEnabled", "(JZ)V",          (void*)am2_void_jz },
    { "nativeGetLastResourceResolution","(J)Ljava/lang/String;",      (void*)am2_lastRes },
    { "nativeAssetDestroy",            "(J)V",                       (void*)am2_void_j },
    { "nativeAssetReadChar",           "(J)I",                       (void*)am2_int_j },
    { "nativeAssetRead",               "(J[BII)I",                   (void*)am2_assetRead },
    { "nativeAssetSeek",               "(JJI)J",                     (void*)am2_long_j },
    { "nativeAssetGetLength",          "(J)J",                        (void*)am2_long_j },
    { "nativeAssetGetRemainingLength", "(J)J",                        (void*)am2_long_j },
    { "nativeGetOverlayableMap",       "(JLjava/lang/String;)Ljava/util/Map;", (void*)am2_map },
    { "nativeGetOverlayablesToString", "(JLjava/lang/String;)Ljava/lang/String;", (void*)am2_lastRes },
};

static const JNINativeMethod kAM[] = {
    { "nativeGetThemeFreeFunction", "()J",          (void*)am_themeFreeFunc },
    { "nativeCreate",                "()J",          (void*)am_nativeCreate },
    { "nativeGetResourceValue",      "(JISLandroid/util/TypedValue;Z)I", (void*)am_getResourceValue },
    { "nativeSetApkAssets",          "(J[Landroid/content/res/ApkAssets;ZZ)V", (void*)am_setApkAssets },
    { "nativeDestroy",               "(J)V",         (void*)am_nativeDestroy },
    /* v13 ★★ theme/style 族（AOSP 36 签名逐字核对） */
    { "nativeThemeCreate",                  "(J)J",                                       (void*)am_themeCreate },
    { "nativeThemeApplyStyle",              "(JJIZ)V",                                    (void*)am_themeApplyStyle },
    { "nativeThemeCopy",                    "(JJJJ)V",                                    (void*)am_themeCopy },
    { "nativeThemeGetAttributeValue",       "(JJILandroid/util/TypedValue;Z)I",          (void*)am_themeGetAttrValue },
    { "nativeThemeGetChangingConfigurations", "(J)I",                                     (void*)am_themeGetChangingCfg },
    { "nativeThemeDump",                    "(JJILjava/lang/String;Ljava/lang/String;)V", (void*)am_themeDump },
    { "nativeGetParentThemeIdentifier",      "(JI)I",                                     (void*)am_themeGetParentThemeId },
    { "nativeApplyStyle",                   "(JJIIJ[IJJ)V",                                (void*)am_applyStyle },
    { "nativeResolveAttrs",                "(JJII[I[I[I[I)Z",                            (void*)am_resolveAttrs },
    { "nativeRetrieveAttributes",           "(JJ[I[I[I)Z",                                (void*)am_retrieveAttributes },
    { "nativeGetResourceIdentifier",        "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)I", (void*)am_getResourceIdentifier },
    { "nativeGetStyleAttributes",           "(JI)[I",                                     (void*)am_getStyleAttributes },
};
static const JNINativeMethod kMQ[] = {
    { "nativeInit",                 "()J",                             (void*)mq_nativeInit },
    { "nativeDestroy",              "(J)V",                            (void*)mq_nativeDestroy },
    { "nativePollOnce",             "(JI)V",                           (void*)mq_nativePollOnce },
    { "nativeWake",                 "(J)V",                            (void*)mq_nativeWake },
    { "nativeSetFileDescriptorEvents", "(JII)V",                      (void*)mq_nativeSetFileDescriptorEvents },
};
static const JNINativeMethod kEL[] = {
    { "writeEvent", "(IF)I",                     (void*)el_writeEvent1 },
    { "writeEvent", "(II)I",                     (void*)el_writeEvent2 },
    { "writeEvent", "(IJ)I",                     (void*)el_writeEvent3 },
    { "writeEvent", "(I[Ljava/lang/Object;)I",  (void*)el_writeEventArr },
    { "writeEvent", "(ILjava/lang/String;)I",    (void*)el_writeEventStr },
};
static const JNINativeMethod kBI[] = {
    { "getContextObject", "()Landroid/os/IBinder;", (void*)bi_getContextObject },
};
static const JNINativeMethod kBinder[] = {
    { "getCallingPid",     "()I", (void*)bd_getCallingPid },
    { "getNativeBBinderHolder", "()J", (void*)bd_nativeBBinderHolder },
    { "getNativeFinalizer", "()J", (void*)bd_nativeGetFinalizer },
    { "getCallingUid",     "()I", (void*)bd_getCallingUid },
};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return -1;
    /* v5b ★：每个 FindClass 后必须清 pending——08:32 案：
     * guest BCP 里没有 BinderInternal（framework.jar 不在 BCP 第 8 件），
     * FindClass 抛 NoClassDef 挂着不清 → 后续 RegisterNatives 全部
     * "unexpected pending exception" → SystemProperties.native_get 缺方法
     * → 裸 abort。JNI 铁律：FindClass 可失败，失败必须 ExceptionClear。 */
    jclass log = (*env)->FindClass(env, "android/util/Log");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass sp = (*env)->FindClass(env, "android/os/SystemProperties");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass tr = (*env)->FindClass(env, "android/os/Trace");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass sc = (*env)->FindClass(env, "android/os/SystemClock");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass bd = (*env)->FindClass(env, "android/os/Binder");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass pn = (*env)->FindClass(env, "android/graphics/Paint");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass tf = (*env)->FindClass(env, "android/graphics/Typeface");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    /* M3-c ③ 修正：真名是 com.android.internal.os.BinderInternal
     *（09:26 案：framework.jar 就在 BCP——两次都打错包名！）*/
    jclass bi = (*env)->FindClass(env, "com/android/internal/os/BinderInternal");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass el = (*env)->FindClass(env, "android/util/EventLog");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass mq = (*env)->FindClass(env, "android/os/MessageQueue");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass am = (*env)->FindClass(env, "android/content/res/AssetManager");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    /* v17：ColorSpace 嵌套 Native 类（ROM 实测名；$ 在 JNI 类名里原样） */
    jclass cs = (*env)->FindClass(env, "android/graphics/ColorSpace$Rgb$Native");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    /* v17 ★★ 图形族（Canvas/draw 阶段）：ROM dex 实测类名 */
    jclass bc = (*env)->FindClass(env, "android/graphics/BaseCanvas");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass cv = (*env)->FindClass(env, "android/graphics/Canvas");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass mx = (*env)->FindClass(env, "android/graphics/Matrix");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass mxe = (*env)->FindClass(env, "android/graphics/Matrix$ExtraNatives");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass pth = (*env)->FindClass(env, "android/graphics/Path");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    jclass rn = (*env)->FindClass(env, "android/graphics/RenderNode");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    /* v4 逐条容错注册（14:54 案教训）*/
    int i;
    jclass cls[] = { log, sp, tr, sc, bd, bi, el, mq, am, rn, pn, tf, am, cs,
                     bc, cv, mx, mxe, pth,
                     tr, sc, bd, bi, el, mq, am, tf };
    /* v16c ★（23:14 案）：cls 只扩了 tabs 没扩——kPaint/kTypeface/kAM2
     * 注册到越界垃圾槽 → AM JNIR 全家失败 → 类链接失败洪流 → SEGV。
     * kAM2 与 kAM 同类（AssetManager）——复用 am 槽。 */
    const JNINativeMethod* tabs[] = { kLog, kSP, kTrace, kSC, kBinder, kBI, kEL, kMQ, kAM,
                                      kGen_android_graphics_RenderNode,
                                      kGen_android_graphics_Paint,
                                      kTypeface, kAM2, kColorSpace,
                                      kGen_android_graphics_BaseCanvas,
                                      kGen_android_graphics_Canvas,
                                      kGen_android_graphics_Matrix,
                                      kGen_android_graphics_Matrix_ExtraNatives,
                                      kGen_android_graphics_Path,
                                      kMiss_android_os_Trace,
                                      kMiss_android_os_SystemClock,
                                      kMiss_android_os_Binder,
                                      kMiss_com_android_internal_os_BinderInternal,
                                      kMiss_android_util_EventLog,
                                      kMiss_android_os_MessageQueue,
                                      kMiss_android_content_res_AssetManager,
                                      kMiss_android_graphics_Typeface };
    int cnts[] = { (int)(sizeof(kLog)/sizeof(kLog[0])),
                   (int)(sizeof(kSP)/sizeof(kSP[0])),
                   (int)(sizeof(kTrace)/sizeof(kTrace[0])),
                   (int)(sizeof(kSC)/sizeof(kSC[0])),
                   (int)(sizeof(kBinder)/sizeof(kBinder[0])),
                   (int)(sizeof(kBI)/sizeof(kBI[0])),
                   (int)(sizeof(kEL)/sizeof(kEL[0])),
                   (int)(sizeof(kMQ)/sizeof(kMQ[0])),
                   (int)(sizeof(kAM)/sizeof(kAM[0])),
                   (int)(sizeof(kGen_android_graphics_RenderNode)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kGen_android_graphics_Paint)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kTypeface)/sizeof(kTypeface[0])),
                   (int)(sizeof(kAM2)/sizeof(kAM2[0])),
                   (int)(sizeof(kColorSpace)/sizeof(kColorSpace[0])),
                   (int)(sizeof(kGen_android_graphics_BaseCanvas)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kGen_android_graphics_Canvas)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kGen_android_graphics_Matrix)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kGen_android_graphics_Matrix_ExtraNatives)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kGen_android_graphics_Path)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kMiss_android_os_Trace)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kMiss_android_os_SystemClock)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kMiss_android_os_Binder)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kMiss_com_android_internal_os_BinderInternal)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kMiss_android_util_EventLog)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kMiss_android_os_MessageQueue)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kMiss_android_content_res_AssetManager)/sizeof(JNINativeMethod)),
                   (int)(sizeof(kMiss_android_graphics_Typeface)/sizeof(JNINativeMethod)) };
    int okReg = 0, badReg = 0;
    for (int t = 0; t < 27; t++) {
        if (!cls[t]) {
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            fprintf(stdout, "[bridge] class[%d] FindClass FAILED（跳过）\n", t);
            continue;
        }
        int badHere = 0;
        for (i = 0; i < cnts[t]; i++) {
            if ((*env)->RegisterNatives(env, cls[t], &tabs[t][i], 1) != 0) {
                (*env)->ExceptionClear(env);
                badHere++; badReg++;
                /* 新族（t>=13）逐条报：签名错位必须可见（发版三关） */
                if (t >= 13 && badHere <= 6)
                    fprintf(stdout, "[bridge]   reg FAIL %s\n", tabs[t][i].name);
            } else okReg++;
        }
        if (badHere && t >= 13)
            fprintf(stdout, "[bridge] table[%d] %d/%d 条注册失败\n", t, badHere, cnts[t]);
    }
    fprintf(stdout, "[bridge] JNI 注册完成: %d ok / %d fail（27 张表）\n", okReg, badReg);
    return JNI_VERSION_1_6;
}
