package com.twinvm.m3;

import android.app.Activity;
import android.app.FragmentHostCallback;
import android.content.Context;
import android.os.Handler;

/** v23：FragmentHostCallback 的最小具体子类（bootstrap 内预编）——
 * Activity.onCreate 的 mFragments.dispatchXxx() 需要非 null host */
public class MyHost extends FragmentHostCallback<Void> {
    public MyHost(Activity act, Context ctx, Handler h, int anim) {
        super(act == null ? ctx : act, h, anim);
    }

    private Activity mAct;
    public void bind(Activity a) { mAct = a; }

    @Override
    public Void onGetHost() {
        return null;
    }

    /* getActivity() 是 FragmentHostCallback 的 final（构造时已传 act）*/
}
