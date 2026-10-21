public class World {
    public static void main(String[] args) {
        System.out.println("world TwinVM 容器B from ART");
        /* 容器B 独立体检：与容器A 不同的计算特征 */
        long t0 = System.currentTimeMillis();
        /* 1. 素数筛（Eratosthenes，200 以内） */
        boolean[] c = new boolean[201];
        int cnt = 0;
        for (int i = 2; i <= 200; i++) {
            if (!c[i]) { cnt++; for (int j = i * i; j <= 200; j += i) c[j] = true; }
        }
        System.out.println("primes(200): " + cnt);
        /* 2. 回文检测 */
        String[] ws = { "level", "hello", "noon", "twinvm", "racecar" };
        int pal = 0;
        for (String w : ws) {
            boolean p = true;
            for (int i = 0, j = w.length() - 1; i < j; i++, j--)
                if (w.charAt(i) != w.charAt(j)) { p = false; break; }
            if (p) pal++;
        }
        System.out.println("palindromes: " + pal + "/" + ws.length);
        /* 3. 矩阵乘（40x40） */
        int n = 40;
        long[][] a = new long[n][n], b = new long[n][n], r = new long[n][n];
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) { a[i][j] = i + j; b[i][j] = i - j; }
        long tr = 0;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                long s = 0;
                for (int k = 0; k < n; k++) s += a[i][k] * b[k][j];
                r[i][j] = s;
                if (i == j) tr += s;
            }
        System.out.println("matrix40-trace: " + tr);
        /* 4. 异常转字符串 */
        try { ((String) null).length(); }
        catch (NullPointerException e) { System.out.println("caught NPE: ok"); }
        /* 5. 线程独立输出 */
        Thread t = new Thread(() -> System.out.println("world-thread: B"));
        t.start();
        try { t.join(); } catch (InterruptedException ignored) {}
        long dt = System.currentTimeMillis() - t0;
        System.out.println("world-all-pass in " + dt + "ms");
    }
}
