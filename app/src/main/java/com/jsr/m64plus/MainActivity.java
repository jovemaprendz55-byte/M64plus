package com.jsr.m64plus;
import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.Toast;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
/** Android entry point; the triangle remains the visible graphics smoke test. */
public final class MainActivity extends Activity {
    private static final String TAG = "M64plus-Native";
    private static final int REQUEST_OPEN_ROM = 6401;
    private static final int MAX_ROM_BYTES = 64 * 1024 * 1024;
    static { System.loadLibrary("m64plus_native"); }
    private GLSurfaceView glSurfaceView;
    private long nativeMachineHandle;
    private final Object nativeMachineLock = new Object();
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
    private static native boolean nativeCpuBusIntegrationSelfTest();
    private static native boolean nativeCpuMiInterruptSelfTest();
    private static native boolean nativeRspMachineSelfTest();
    private static native long nativeCreateMachine();
    private static native void nativeDestroyMachine(long handle);
    private static native boolean nativeResetMachine(long handle);
    private static native long nativeMachineCpuPc(long handle);
    static native int[] nativeGetFramebufferSnapshot(long handle);
    private static native boolean nativeLoadCartridge(long handle, byte[] romData);
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
        boolean cpuBusPassed = nativeCpuBusIntegrationSelfTest();
        boolean miInterruptPassed = nativeCpuMiInterruptSelfTest();
        boolean rspMachinePassed = nativeRspMachineSelfTest();
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
        Log.i(TAG, "MIPS CPU + physical bus integration self-test: " + (cpuBusPassed ? "PASS" : "FAIL"));
        Log.i(TAG, "MI hardware interrupt / CP0 delivery self-test: " + (miInterruptPassed ? "PASS" : "FAIL"));
        Log.i(TAG, "MIPS CPU + RSP shared-machine self-test: " + (rspMachinePassed ? "PASS" : "FAIL"));
        nativeMachineHandle = nativeCreateMachine();
        if (nativeMachineHandle != 0L) {
            Log.i(TAG, "Persistent N64Machine: CREATED; CPU_PC=0x" + Long.toHexString(nativeMachineCpuPc(nativeMachineHandle)));
        } else {
            Log.e(TAG, "Persistent N64Machine: CREATE_FAILED");
        }
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN, WindowManager.LayoutParams.FLAG_FULLSCREEN);
        glSurfaceView = new GLSurfaceView(this);
        glSurfaceView.setEGLContextClientVersion(2);
        glSurfaceView.setPreserveEGLContextOnPause(true);
        glSurfaceView.setRenderer(new BootRenderer(nativeMachineHandle, nativeMachineLock));
        glSurfaceView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        glSurfaceView.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        FrameLayout root = new FrameLayout(this);
        root.addView(glSurfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        Button openRomButton = new Button(this);
        openRomButton.setText("ABRIR ROM (.z64)");
        openRomButton.setOnClickListener(view -> openRomPicker());
        FrameLayout.LayoutParams buttonParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
                android.view.Gravity.TOP | android.view.Gravity.START);
        int margin = (int) (16 * getResources().getDisplayMetrics().density);
        buttonParams.setMargins(margin, margin, margin, margin);
        root.addView(openRomButton, buttonParams);
        root.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        setContentView(root);
    }
    private void openRomPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, REQUEST_OPEN_ROM);
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_OPEN_ROM && resultCode == RESULT_OK &&
                data != null && data.getData() != null) {
            loadRomInBackground(data.getData());
        }
    }

    private void loadRomInBackground(Uri uri) {
        new Thread(() -> {
            String message;
            try (InputStream input = getContentResolver().openInputStream(uri);
                 ByteArrayOutputStream output = new ByteArrayOutputStream()) {
                if (input == null) throw new IOException("Não foi possível abrir o arquivo");
                byte[] buffer = new byte[64 * 1024];
                int total = 0;
                int count;
                while ((count = input.read(buffer)) != -1) {
                    total += count;
                    if (total > MAX_ROM_BYTES) throw new IOException("ROM maior que 64 MiB");
                    output.write(buffer, 0, count);
                }
                byte[] romData = output.toByteArray();
                boolean loaded;
                synchronized (nativeMachineLock) {
                    loaded = nativeMachineHandle != 0L &&
                            nativeLoadCartridge(nativeMachineHandle, romData);
                }
                message = loaded
                        ? "ROM .z64 carregada. Inicialização do jogo ainda não implementada."
                        : "ROM inválida. Use uma ROM .z64 big-endian.";
                Log.i(TAG, "Cartridge load: " + (loaded ? "SUCCESS" : "REJECTED") +
                        "; bytes=" + romData.length);
            } catch (IOException exception) {
                message = "Erro ao abrir ROM: " + exception.getMessage();
                Log.e(TAG, message, exception);
            }
            final String resultMessage = message;
            runOnUiThread(() -> Toast.makeText(MainActivity.this, resultMessage,
                    Toast.LENGTH_LONG).show());
        }, "M64plus-ROM-loader").start();
    }

    @Override protected void onDestroy() {
        if (glSurfaceView != null) glSurfaceView.onPause();
        synchronized (nativeMachineLock) {
            if (nativeMachineHandle != 0L) {
                nativeDestroyMachine(nativeMachineHandle);
                nativeMachineHandle = 0L;
            }
        }
        super.onDestroy();
    }
    @Override protected void onPause() { if (glSurfaceView != null) glSurfaceView.onPause(); super.onPause(); }
    @Override protected void onResume() { super.onResume(); if (glSurfaceView != null) glSurfaceView.onResume(); }
}
