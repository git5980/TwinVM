package com.twinvm.m3;

/** 自动生成（tools/gen_pkgmgr.py）：PackageManager 最小壳。
 *  guest 无真实 PackageManager：hasSystemFeature=false、查询为空、包信息最小化。 */
public class MiniPackageManager extends android.content.pm.PackageManager {

    static android.content.pm.ApplicationInfo miniApplicationInfo(String pkg) {
        android.content.pm.ApplicationInfo ai = new android.content.pm.ApplicationInfo();
        ai.packageName = pkg;
        ai.uid = 2000;
        ai.flags = 0;
        ai.sourceDir = "/data/app/TwinVM-M2-DemoApp.apk";
        ai.dataDir = "/data/data/" + pkg;
        ai.targetSdkVersion = 36;
        ai.minSdkVersion = 24;
        return ai;
    }

    static android.content.pm.PackageInfo miniPackageInfo(String pkg) {
        android.content.pm.PackageInfo pi = new android.content.pm.PackageInfo();
        pi.packageName = pkg;
        pi.versionName = "1.0";
        pi.versionCode = 1;
        pi.applicationInfo = miniApplicationInfo(pkg);
        return pi;
    }

    @Override public void addPackageToPreferred(java.lang.String a0) {
    }
    @Override public boolean addPermission(android.content.pm.PermissionInfo a0) {
        return false;
    }
    @Override public boolean addPermissionAsync(android.content.pm.PermissionInfo a0) {
        return false;
    }
    @Override public void addPreferredActivity(android.content.IntentFilter a0, int a1, android.content.ComponentName[] a2, android.content.ComponentName a3) {
    }
    @Override public boolean canRequestPackageInstalls() {
        return false;
    }
    @Override public java.lang.String[] canonicalToCurrentPackageNames(java.lang.String[] a0) {
        return new String[0];
    }
    @Override public int checkPermission(java.lang.String a0, java.lang.String a1) {
        return android.content.pm.PackageManager.PERMISSION_DENIED;
    }
    @Override public int checkSignatures(int a0, int a1) {
        return android.content.pm.PackageManager.SIGNATURE_NO_MATCH;
    }
    @Override public int checkSignatures(java.lang.String a0, java.lang.String a1) {
        return android.content.pm.PackageManager.SIGNATURE_NO_MATCH;
    }
    @Override public void clearInstantAppCookie() {
    }
    @Override public void clearPackagePreferredActivities(java.lang.String a0) {
    }
    @Override public java.lang.String[] currentToCanonicalPackageNames(java.lang.String[] a0) {
        return new String[0];
    }
    @Override public void extendVerificationTimeout(int a0, int a1, long a2) {
    }
    @Override public android.graphics.drawable.Drawable getActivityBanner(android.content.ComponentName a0) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.graphics.drawable.Drawable getActivityBanner(android.content.Intent a0) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.graphics.drawable.Drawable getActivityIcon(android.content.ComponentName a0) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.graphics.drawable.Drawable getActivityIcon(android.content.Intent a0) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.content.pm.ActivityInfo getActivityInfo(android.content.ComponentName a0, int a1) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.graphics.drawable.Drawable getActivityLogo(android.content.ComponentName a0) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.graphics.drawable.Drawable getActivityLogo(android.content.Intent a0) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public java.util.List<android.content.pm.PermissionGroupInfo> getAllPermissionGroups(int a0) {
        return new java.util.ArrayList<>();
    }
    @Override public android.graphics.drawable.Drawable getApplicationBanner(android.content.pm.ApplicationInfo a0) {
        return null;
    }
    @Override public android.graphics.drawable.Drawable getApplicationBanner(java.lang.String a0) throws android.content.pm.PackageManager.NameNotFoundException {
        return null;
    }
    @Override public int getApplicationEnabledSetting(java.lang.String a0) {
        return android.content.pm.PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }
    @Override public android.graphics.drawable.Drawable getApplicationIcon(android.content.pm.ApplicationInfo a0) {
        return null;
    }
    @Override public android.graphics.drawable.Drawable getApplicationIcon(java.lang.String a0) throws android.content.pm.PackageManager.NameNotFoundException {
        return null;
    }
    @Override public android.content.pm.ApplicationInfo getApplicationInfo(java.lang.String a0, int a1) throws android.content.pm.PackageManager.NameNotFoundException {
        return miniApplicationInfo(a0);
    }
    @Override public java.lang.CharSequence getApplicationLabel(android.content.pm.ApplicationInfo a0) {
        return null;
    }
    @Override public android.graphics.drawable.Drawable getApplicationLogo(android.content.pm.ApplicationInfo a0) {
        return null;
    }
    @Override public android.graphics.drawable.Drawable getApplicationLogo(java.lang.String a0) throws android.content.pm.PackageManager.NameNotFoundException {
        return null;
    }
    @Override public android.content.pm.ChangedPackages getChangedPackages(int a0) {
        return null;
    }
    @Override public int getComponentEnabledSetting(android.content.ComponentName a0) {
        return android.content.pm.PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }
    @Override public android.graphics.drawable.Drawable getDefaultActivityIcon() {
        return null;
    }
    @Override public android.graphics.drawable.Drawable getDrawable(java.lang.String a0, int a1, android.content.pm.ApplicationInfo a2) {
        return null;
    }
    @Override public java.util.List<android.content.pm.ApplicationInfo> getInstalledApplications(int a0) {
        return new java.util.ArrayList<>();
    }
    @Override public java.util.List<android.content.pm.PackageInfo> getInstalledPackages(int a0) {
        return new java.util.ArrayList<>();
    }
    @Override public java.lang.String getInstallerPackageName(java.lang.String a0) {
        return null;
    }
    @Override public byte[] getInstantAppCookie() {
        return new byte[0];
    }
    @Override public int getInstantAppCookieMaxBytes() {
        return 0;
    }
    @Override public android.content.pm.InstrumentationInfo getInstrumentationInfo(android.content.ComponentName a0, int a1) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.content.Intent getLaunchIntentForPackage(java.lang.String a0) {
        return null;
    }
    @Override public android.content.Intent getLeanbackLaunchIntentForPackage(java.lang.String a0) {
        return null;
    }
    @Override public java.lang.String getNameForUid(int a0) {
        return null;
    }
    @Override public int[] getPackageGids(java.lang.String a0) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public int[] getPackageGids(java.lang.String a0, int a1) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.content.pm.PackageInfo getPackageInfo(android.content.pm.VersionedPackage a0, int a1) throws android.content.pm.PackageManager.NameNotFoundException {
        return miniPackageInfo(a0.getPackageName());
    }
    @Override public android.content.pm.PackageInfo getPackageInfo(java.lang.String a0, int a1) throws android.content.pm.PackageManager.NameNotFoundException {
        return miniPackageInfo(a0);
    }
    @Override public android.content.pm.PackageInstaller getPackageInstaller() {
        return null;
    }
    @Override public int getPackageUid(java.lang.String a0, int a1) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public java.lang.String[] getPackagesForUid(int a0) {
        return new String[0];
    }
    @Override public java.util.List<android.content.pm.PackageInfo> getPackagesHoldingPermissions(java.lang.String[] a0, int a1) {
        return new java.util.ArrayList<>();
    }
    @Override public android.content.pm.PermissionGroupInfo getPermissionGroupInfo(java.lang.String a0, int a1) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.content.pm.PermissionInfo getPermissionInfo(java.lang.String a0, int a1) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public int getPreferredActivities(java.util.List<android.content.IntentFilter> a0, java.util.List<android.content.ComponentName> a1, java.lang.String a2) {
        return 0;
    }
    @Override public java.util.List<android.content.pm.PackageInfo> getPreferredPackages(int a0) {
        return new java.util.ArrayList<>();
    }
    @Override public android.content.pm.ProviderInfo getProviderInfo(android.content.ComponentName a0, int a1) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.content.pm.ActivityInfo getReceiverInfo(android.content.ComponentName a0, int a1) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.content.res.Resources getResourcesForActivity(android.content.ComponentName a0) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.content.res.Resources getResourcesForApplication(android.content.pm.ApplicationInfo a0) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.content.res.Resources getResourcesForApplication(java.lang.String a0) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public android.content.pm.ServiceInfo getServiceInfo(android.content.ComponentName a0, int a1) throws android.content.pm.PackageManager.NameNotFoundException {
        throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(a0));
    }
    @Override public java.util.List<android.content.pm.SharedLibraryInfo> getSharedLibraries(int a0) {
        return new java.util.ArrayList<>();
    }
    @Override public android.content.pm.FeatureInfo[] getSystemAvailableFeatures() {
        return new android.content.pm.FeatureInfo[0];
    }
    @Override public java.lang.String[] getSystemSharedLibraryNames() {
        return new String[0];
    }
    @Override public java.lang.CharSequence getText(java.lang.String a0, int a1, android.content.pm.ApplicationInfo a2) {
        return null;
    }
    @Override public android.graphics.drawable.Drawable getUserBadgedDrawableForDensity(android.graphics.drawable.Drawable a0, android.os.UserHandle a1, android.graphics.Rect a2, int a3) {
        return null;
    }
    @Override public android.graphics.drawable.Drawable getUserBadgedIcon(android.graphics.drawable.Drawable a0, android.os.UserHandle a1) {
        return null;
    }
    @Override public java.lang.CharSequence getUserBadgedLabel(java.lang.CharSequence a0, android.os.UserHandle a1) {
        return null;
    }
    @Override public android.content.res.XmlResourceParser getXml(java.lang.String a0, int a1, android.content.pm.ApplicationInfo a2) {
        return null;
    }
    @Override public boolean hasSystemFeature(java.lang.String a0) {
        return false;
    }
    @Override public boolean hasSystemFeature(java.lang.String a0, int a1) {
        return false;
    }
    @Override public boolean isInstantApp() {
        return false;
    }
    @Override public boolean isInstantApp(java.lang.String a0) {
        return false;
    }
    @Override public boolean isPermissionRevokedByPolicy(java.lang.String a0, java.lang.String a1) {
        return false;
    }
    @Override public boolean isSafeMode() {
        return false;
    }
    @Override public java.util.List<android.content.pm.ResolveInfo> queryBroadcastReceivers(android.content.Intent a0, int a1) {
        return new java.util.ArrayList<>();
    }
    @Override public java.util.List<android.content.pm.ProviderInfo> queryContentProviders(java.lang.String a0, int a1, int a2) {
        return new java.util.ArrayList<>();
    }
    @Override public java.util.List<android.content.pm.InstrumentationInfo> queryInstrumentation(java.lang.String a0, int a1) {
        return new java.util.ArrayList<>();
    }
    @Override public java.util.List<android.content.pm.ResolveInfo> queryIntentActivities(android.content.Intent a0, int a1) {
        return new java.util.ArrayList<>();
    }
    @Override public java.util.List<android.content.pm.ResolveInfo> queryIntentActivityOptions(android.content.ComponentName a0, android.content.Intent[] a1, android.content.Intent a2, int a3) {
        return new java.util.ArrayList<>();
    }
    @Override public java.util.List<android.content.pm.ResolveInfo> queryIntentContentProviders(android.content.Intent a0, int a1) {
        return new java.util.ArrayList<>();
    }
    @Override public java.util.List<android.content.pm.ResolveInfo> queryIntentServices(android.content.Intent a0, int a1) {
        return new java.util.ArrayList<>();
    }
    @Override public java.util.List<android.content.pm.PermissionInfo> queryPermissionsByGroup(java.lang.String a0, int a1) throws android.content.pm.PackageManager.NameNotFoundException {
        return new java.util.ArrayList<>();
    }
    @Override public void removePackageFromPreferred(java.lang.String a0) {
    }
    @Override public void removePermission(java.lang.String a0) {
    }
    @Override public android.content.pm.ResolveInfo resolveActivity(android.content.Intent a0, int a1) {
        return null;
    }
    @Override public android.content.pm.ProviderInfo resolveContentProvider(java.lang.String a0, int a1) {
        return null;
    }
    @Override public android.content.pm.ResolveInfo resolveService(android.content.Intent a0, int a1) {
        return null;
    }
    @Override public void setApplicationCategoryHint(java.lang.String a0, int a1) {
    }
    @Override public void setApplicationEnabledSetting(java.lang.String a0, int a1, int a2) {
    }
    @Override public void setComponentEnabledSetting(android.content.ComponentName a0, int a1, int a2) {
    }
    @Override public void setInstallerPackageName(java.lang.String a0, java.lang.String a1) {
    }
    @Override public void updateInstantAppCookie(byte[] a0) {
    }
    @Override public void verifyPendingInstall(int a0, int a1) {
    }
}
