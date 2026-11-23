package com.jsr.m64plus;

import android.opengl.EGL14;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.util.Log;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.ArrayList;
import java.util.List;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/** GLES renderer: displays a valid native framebuffer snapshot, otherwise the diagnostic triangle. */
final class BootRenderer implements GLSurfaceView.Renderer {
    private static final String TAG = "M64plus-GPU";
    private static final String TRIANGLE_VERTEX =
            "attribute vec4 aPosition; attribute vec4 aColor; varying vec4 vColor;" +
            "void main(){gl_Position=aPosition;vColor=aColor;}";
    private static final String TRIANGLE_FRAGMENT =
            "precision mediump float; varying vec4 vColor;" +
            "void main(){gl_FragColor=vColor;}";
    private static final String TEXTURE_VERTEX =
            "attribute vec2 aPosition; attribute vec2 aTexCoord; varying vec2 vTexCoord;" +
            "void main(){gl_Position=vec4(aPosition,0.0,1.0);vTexCoord=aTexCoord;}";
    private static final String TEXTURE_FRAGMENT =
            "precision mediump float; varying vec2 vTexCoord; uniform sampler2D uTexture;" +
            "void main(){gl_FragColor=texture2D(uTexture,vTexCoord);}";
    private static final String[] IMPORTANT_EXTENSIONS = {
            "GL_OES_EGL_image", "GL_OES_EGL_image_external",
            "GL_KHR_parallel_shader_compile", "GL_ARB_parallel_shader_compile",
            "GL_EXT_buffer_storage", "GL_EXT_disjoint_timer_query",
            "GL_EXT_color_buffer_float", "GL_EXT_multisampled_render_to_texture",
            "GL_OES_mapbuffer", "GL_EXT_map_buffer_range",
            "GL_OES_vertex_array_object", "GL_EXT_texture_filter_anisotropic"
    };
    private static final float[] TRIANGLE = {
             0.0f,  0.62f, 1.0f, 0.25f, 0.18f, 1.0f,
            -0.58f, -0.42f, 0.18f, 0.75f, 1.0f, 1.0f,
             0.58f, -0.42f, 0.25f, 1.0f, 0.35f, 1.0f
    };
    private static final float[] QUAD = {
            -1f,-1f, 0f,1f,  1f,-1f, 1f,1f,
            -1f, 1f, 0f,0f,  1f, 1f, 1f,0f
    };

    private final long machineHandle;
    private final Object machineLock;
    private final FloatBuffer triangleBuffer;
    private final FloatBuffer quadBuffer;
    private int triangleProgram;
    private int textureProgram;
    private int textureId;
    private int positionHandle;
    private int colorHandle;
    private int quadPositionHandle;
    private int quadTexCoordHandle;
    private int samplerHandle;
    private int textureWidth;
    private int textureHeight;
    private boolean loggedFirstFrame;

    BootRenderer(long handle, Object lock) {
        machineHandle = handle;
        machineLock = lock;
        triangleBuffer = floatBuffer(TRIANGLE);
        quadBuffer = floatBuffer(QUAD);
    }

    private static FloatBuffer floatBuffer(float[] values) {
        ByteBuffer bytes = ByteBuffer.allocateDirect(values.length * 4).order(ByteOrder.nativeOrder());
        FloatBuffer buffer = bytes.asFloatBuffer();
        buffer.put(values);
        buffer.position(0);
        return buffer;
    }

    @Override public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        logGraphicsCapabilities();
        GLES20.glClearColor(0.035f, 0.045f, 0.075f, 1f);
        GLES20.glDisable(GLES20.GL_DEPTH_TEST);
        triangleProgram = linkProgram(TRIANGLE_VERTEX, TRIANGLE_FRAGMENT);
        positionHandle = GLES20.glGetAttribLocation(triangleProgram, "aPosition");
        colorHandle = GLES20.glGetAttribLocation(triangleProgram, "aColor");
        textureProgram = linkProgram(TEXTURE_VERTEX, TEXTURE_FRAGMENT);
        quadPositionHandle = GLES20.glGetAttribLocation(textureProgram, "aPosition");
        quadTexCoordHandle = GLES20.glGetAttribLocation(textureProgram, "aTexCoord");
        samplerHandle = GLES20.glGetUniformLocation(textureProgram, "uTexture");
        int[] textures = new int[1];
        GLES20.glGenTextures(1, textures, 0);
        textureId = textures[0];
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_NEAREST);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
    }

    private static int linkProgram(String vertexSource, String fragmentSource) {
        int vertex = compileShader(GLES20.GL_VERTEX_SHADER, vertexSource);
        int fragment = compileShader(GLES20.GL_FRAGMENT_SHADER, fragmentSource);
        int program = GLES20.glCreateProgram();
        GLES20.glAttachShader(program, vertex);
        GLES20.glAttachShader(program, fragment);
        GLES20.glLinkProgram(program);
        int[] status = new int[1];
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, status, 0);
        GLES20.glDeleteShader(vertex);
        GLES20.glDeleteShader(fragment);
        if (status[0] == 0) {
            String error = GLES20.glGetProgramInfoLog(program);
            GLES20.glDeleteProgram(program);
            throw new IllegalStateException("OpenGL program link failed: " + error);
        }
        return program;
    }

    private static int compileShader(int type, String source) {
        int shader = GLES20.glCreateShader(type);
        GLES20.glShaderSource(shader, source);
        GLES20.glCompileShader(shader);
        int[] status = new int[1];
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0);
        if (status[0] == 0) {
            String error = GLES20.glGetShaderInfoLog(shader);
            GLES20.glDeleteShader(shader);
            throw new IllegalStateException("OpenGL shader compile failed: " + error);
        }
        return shader;
    }

    private static void logGraphicsCapabilities() {
        String vendor = GLES20.glGetString(GLES20.GL_VENDOR);
        String renderer = GLES20.glGetString(GLES20.GL_RENDERER);
        String version = GLES20.glGetString(GLES20.GL_VERSION);
        String extensions = GLES20.glGetString(GLES20.GL_EXTENSIONS);
        Log.i(TAG, "GL_VENDOR=" + vendor);
        Log.i(TAG, "GL_RENDERER=" + renderer);
        Log.i(TAG, "GL_VERSION=" + version);
        List<String> supported = new ArrayList<>();
        if (extensions != null) for (String extension : IMPORTANT_EXTENSIONS) {
            if ((" " + extensions + " ").contains(" " + extension + " ")) supported.add(extension);
        }
        Log.i(TAG, "IMPORTANT_GL_EXTENSIONS=" + supported);
        try {
            android.opengl.EGLDisplay display = EGL14.eglGetCurrentDisplay();
            Log.i(TAG, "EGL_EXTENSIONS=" + EGL14.eglQueryString(display, EGL14.EGL_EXTENSIONS));
        } catch (RuntimeException error) {
            Log.w(TAG, "Could not query EGL extensions", error);
        }
    }

    @Override public void onSurfaceChanged(GL10 gl, int width, int height) {
        GLES20.glViewport(0, 0, width, height);
        Log.i(TAG, "SURFACE_SIZE=" + width + "x" + height);
    }

    @Override public void onDrawFrame(GL10 gl) {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        int[] snapshot = null;
        synchronized (machineLock) {
            if (machineHandle != 0L) snapshot = MainActivity.nativeGetFramebufferSnapshot(machineHandle);
        }
        if (snapshot != null && snapshot.length >= 3) {
            int width = snapshot[0];
            int height = snapshot[1];
            long expected = 2L + (long) width * height;
            if (width > 0 && height > 0 && expected == snapshot.length &&
                    width <= 1024 && height <= 1024) {
                uploadSnapshot(snapshot, width, height);
                drawTexture();
                if (!loggedFirstFrame) {
                    Log.i(TAG, "NATIVE_FRAMEBUFFER_DISPLAYED=" + width + "x" + height);
                    loggedFirstFrame = true;
                }
                return;
            }
        }
        drawDiagnosticTriangle();
    }

    private void uploadSnapshot(int[] snapshot, int width, int height) {
        if (textureWidth == width && textureHeight == height) {
            // The texture storage is reused; only pixel contents change.
        } else {
            textureWidth = width;
            textureHeight = height;
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId);
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA,
                    width, height, 0, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null);
        }
        ByteBuffer rgba = ByteBuffer.allocateDirect(width * height * 4).order(ByteOrder.nativeOrder());
        for (int i = 2; i < snapshot.length; i++) {
            int argb = snapshot[i];
            rgba.put((byte) ((argb >> 16) & 0xFF));
            rgba.put((byte) ((argb >> 8) & 0xFF));
            rgba.put((byte) (argb & 0xFF));
            rgba.put((byte) ((argb >>> 24) & 0xFF));
        }
        rgba.position(0);
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId);
        GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, width, height,
                GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, rgba);
    }

    private void drawTexture() {
        GLES20.glUseProgram(textureProgram);
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId);
        GLES20.glUniform1i(samplerHandle, 0);
        quadBuffer.position(0);
        GLES20.glEnableVertexAttribArray(quadPositionHandle);
        GLES20.glVertexAttribPointer(quadPositionHandle, 2, GLES20.GL_FLOAT, false, 4 * 4, quadBuffer);
        quadBuffer.position(2);
        GLES20.glEnableVertexAttribArray(quadTexCoordHandle);
        GLES20.glVertexAttribPointer(quadTexCoordHandle, 2, GLES20.GL_FLOAT, false, 4 * 4, quadBuffer);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        GLES20.glDisableVertexAttribArray(quadPositionHandle);
        GLES20.glDisableVertexAttribArray(quadTexCoordHandle);
    }

    private void drawDiagnosticTriangle() {
        GLES20.glUseProgram(triangleProgram);
        triangleBuffer.position(0);
        GLES20.glEnableVertexAttribArray(positionHandle);
        GLES20.glVertexAttribPointer(positionHandle, 2, GLES20.GL_FLOAT, false, 6 * 4, triangleBuffer);
        triangleBuffer.position(2);
        GLES20.glEnableVertexAttribArray(colorHandle);
        GLES20.glVertexAttribPointer(colorHandle, 4, GLES20.GL_FLOAT, false, 6 * 4, triangleBuffer);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3);
        GLES20.glDisableVertexAttribArray(positionHandle);
        GLES20.glDisableVertexAttribArray(colorHandle);
    }
}
