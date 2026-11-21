package com.jsr.m64plus;
import android.app.Activity;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
/** Android entry point; the triangle remains the visible graphics smoke test. */
public final class MainActivity extends Activity {
    private static final String TAG = "M64plus-Native";
    static { System.loadLibrary("m64plus_native"); }
    private GLSurfaceView glSurfaceView;
    private static native boolean nativeRdramSelfTest();
    private static native boolean nativeCpuSelfTest();
    private static native boolean nativeCpuBranchSelfTest();
    private static native boolean nativeCpuMemorySelfTest();
    private static native boolean nativeCp0SelfTest();
    private static native boolean nativeCpuExceptionIntegrationSelfTest();
    private static native boolean nativeCpuTlbInstructionSelfTest();
    private static native boolean nativeCpuTlbTranslationSelfTest();
    private static native boolean nativeCpuTlbProtectionSelfTest();
    private static native boolean nativeBusSelfTest();
    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        boolean memoryPassed = nativeRdramSelfTest();
        boolean cpuPassed = nativeCpuSelfTest();
        boolean branchPassed = nativeCpuBranchSelfTest();
        boolean memoryCpuPassed = nativeCpuMemorySelfTest();
        boolean cp0Passed = nativeCp0SelfTest();
        boolean exceptionIntegrationPassed = nativeCpuExceptionIntegrationSelfTest();
        boolean tlbInstructionsPassed = nativeCpuTlbInstructionSelfTest();
        boolean tlbTranslationPassed = nativeCpuTlbTranslationSelfTest();
        boolean tlbProtectionPassed = nativeCpuTlbProtectionSelfTest();
        boolean busPassed = nativeBusSelfTest();
        Log.i(TAG, "RDRAM 8 MiB self-test: " + (memoryPassed ? "PASS" : "FAIL"));
        Log.i(TAG, "MIPS CPU instruction self-test: " + (cpuPassed ? "PASS" : "FAIL"));
        Log.i(TAG, "MIPS branch/delay-slot self-test: " + (branchPassed ? "PASS" : "FAIL"));
        Log.i(TAG, "MIPS load/store self-test: " + (memoryCpuPassed ? "PASS" : "FAIL"));
        Log.i(TAG, "CP0 exception/timer self-test: " + (cp0Passed ? "PASS" : "FAIL"));
        Log.i(TAG, "CPU/CP0 exception integration self-test: " + (exceptionIntegrationPassed ? "PASS" : "FAIL"));
        Log.i(TAG, "MIPS TLB instruction self-test: " + (tlbInstructionsPassed ? "PASS" : "FAIL"));
        Log.i(TAG, "CPU virtual TLB translation self-test: " + (tlbTranslationPassed ? "PASS" : "FAIL"));
        Log.i(TAG, "CPU TLB page-protection self-test: " + (tlbProtectionPassed ? "PASS" : "FAIL"));
        Log.i(TAG, "N64 physical bus / SP memory self-test: " + (busPassed ? "PASS" : "FAIL"));
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN, WindowManager.LayoutParams.FLAG_FULLSCREEN);
        glSurfaceView = new GLSurfaceView(this);
        glSurfaceView.setEGLContextClientVersion(2);
        glSurfaceView.setPreserveEGLContextOnPause(true);
        glSurfaceView.setRenderer(new BootRenderer());
        glSurfaceView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        glSurfaceView.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        setContentView(glSurfaceView);
    }
    @Override protected void onPause() { if (glSurfaceView != null) glSurfaceView.onPause(); super.onPause(); }
    @Override protected void onResume() { super.onResume(); if (glSurfaceView != null) glSurfaceView.onResume(); }
}
