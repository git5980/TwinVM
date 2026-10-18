import java.util.ArrayList;

public class Hello {
    public static void main(String[] args) {
        System.out.println("hello TwinVM from ART");
        Stress.run();
    }
}

class Stress {
    static class Inner {
        int v;
        Inner(int v) { this.v = v; }
        int triple() { return v * 3; }
    }
    static long fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

    static void run() {
        long t0 = System.currentTimeMillis();
        /* 1. 循环 + 算术（10 万次） */
        long sum = 0;
        for (int i = 1; i <= 100000; i++) sum += (long) i * i;
        System.out.println("loop-sum: " + sum);
        /* 2. 字符串（StringBuilder + substring + hashCode） */
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 200; i++) sb.append((char) ('0' + (i % 10)));
        String s = sb.toString();
        System.out.println("str(" + s.length() + "): " + s.substring(0, 16) + " hc=" + s.hashCode());
        /* 3. 集合（ArrayList + 装箱） */
        ArrayList<Object> list = new ArrayList<>();
        for (int i = 0; i < 500; i++) list.add(Integer.valueOf(i));
        long ls = 0;
        for (Object o : list) ls += ((Integer) o).intValue();
        System.out.println("list-sum: " + ls);
        /* 4. 递归（fib 22） */
        System.out.println("fib(22): " + fib(22));
        /* 5. 异常（try/catch/finally） */
        try {
            Object[] a = new Object[2];
            a[5] = null;
            System.out.println("!! no-throw");
        } catch (ArrayIndexOutOfBoundsException e) {
            System.out.println("caught AIOOBE: " + e.getMessage());
        } finally {
            System.out.println("finally-ok");
        }
        /* 6. 内部类 */
        Inner in = new Inner(21);
        System.out.println("inner: " + in.triple());
        /* 7. 多线程（匿名 Runnable + join） */
        try {
            final long[] holder = new long[1];
            Thread t = new Thread(new Runnable() {
                public void run() { holder[0] = 42; }
            });
            t.start();
            t.join(2000);
            System.out.println("thread: " + holder[0]);
        } catch (InterruptedException e) {
            System.out.println("thread-interrupted");
        }
        System.out.println("stress-all-pass in " + (System.currentTimeMillis() - t0) + "ms");
    }
}
