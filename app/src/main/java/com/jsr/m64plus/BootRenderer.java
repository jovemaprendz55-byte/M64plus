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

/**
 * Minimal GLES 2.0 bring-up renderer.
 * Draws a colored triangle and reports the active GPU/driver capabilities.
 */
final class BootRenderer implements GLSurfaceView.Renderer {
    private static final String TAG = "M64plus-GPU";

    private static final String VERTEX_SHADER =
            "attribute vec4 aPosition;\n" +
            "attribute vec4 aColor;\n" +
            "varying vec4 vColor;\n" +
            "void main() {\n" +
            "  gl_Position = aPosition;\n" +
            "  vColor = aColor;\n" +
            "}\n";

    private static final String FRAGMENT_SHADER =
            "precision mediump float;\n" +
            "varying vec4 vColor;\n" +
            "void main() {\n" +
            "  gl_FragColor = vColor;\n" +
            "}\n";

    private static final String[] IMPORTANT_EXTENSIONS = {
            "GL_OES_EGL_image",
            "GL_OES_EGL_image_external",
            "GL_KHR_parallel_shader_compile",
            "GL_ARB_parallel_shader_compile",
            "GL_EXT_buffer_storage",
            "GL_EXT_disjoint_timer_query",
            "GL_EXT_color_buffer_float",
            "GL_EXT_multisampled_render_to_texture",
            "GL_OES_mapbuffer",
            "GL_EXT_map_buffer_range",
            "GL_OES_vertex_array_object",
            "GL_EXT_texture_filter_anisotropic"
    };

    private static final float[] TRIANGLE = {
             0.0f,  0.62f, 1.0f, 0.25f, 0.18f, 1.0f,
            -0.58f, -0.42f, 0.18f, 0.75f, 1.0f, 1.0f,
             0.58f, -0.42f, 0.25f, 1.0f, 0.35f, 1.0f
    };

    private final FloatBuffer vertexBuffer;
    private int program;
    private int positionHandle;
    private int colorHandle;

    BootRenderer() {
        ByteBuffer bytes = ByteBuffer.allocateDirect(TRIANGLE.length * 4);
        bytes.order(ByteOrder.nativeOrder());
        vertexBuffer = bytes.asFloatBuffer();
        vertexBuffer.put(TRIANGLE);
        vertexBuffer.position(0);
    }

    @Override
    public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        logGraphicsCapabilities();

        GLES20.glClearColor(0.035f, 0.045f, 0.075f, 1.0f);
        GLES20.glDisable(GLES20.GL_DEPTH_TEST);

        int vertex = compileShader(GLES20.GL_VERTEX_SHADER, VERTEX_SHADER);
        int fragment = compileShader(GLES20.GL_FRAGMENT_SHADER, FRAGMENT_SHADER);
        program = GLES20.glCreateProgram();
        GLES20.glAttachShader(program, vertex);
        GLES20.glAttachShader(program, fragment);
        GLES20.glLinkProgram(program);

        int[] linkStatus = new int[1];
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, linkStatus, 0);
        if (linkStatus[0] == 0) {
            String error = GLES20.glGetProgramInfoLog(program);
            GLES20.glDeleteProgram(program);
            program = 0;
            throw new IllegalStateException("OpenGL program link failed: " + error);
        }

        GLES20.glDeleteShader(vertex);
        GLES20.glDeleteShader(fragment);
        positionHandle = GLES20.glGetAttribLocation(program, "aPosition");
        colorHandle = GLES20.glGetAttribLocation(program, "aColor");
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
        if (extensions != null) {
            for (String extension : IMPORTANT_EXTENSIONS) {
                if ((" " + extensions + " ").contains(" " + extension + " ")) {
                    supported.add(extension);
                }
            }
        }
        Log.i(TAG, "IMPORTANT_GL_EXTENSIONS=" + supported);

        try {
            android.opengl.EGLDisplay display = EGL14.eglGetCurrentDisplay();
            String eglExtensions = EGL14.eglQueryString(display, EGL14.EGL_EXTENSIONS);
            Log.i(TAG, "EGL_EXTENSIONS=" + eglExtensions);
        } catch (RuntimeException error) {
            Log.w(TAG, "Could not query EGL extensions", error);
        }
    }

    @Override
    public void onSurfaceChanged(GL10 gl, int width, int height) {
        GLES20.glViewport(0, 0, width, height);
        Log.i(TAG, "SURFACE_SIZE=" + width + "x" + height);
    }

    @Override
    public void onDrawFrame(GL10 gl) {
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        if (program == 0) {
            return;
        }

        GLES20.glUseProgram(program);
        vertexBuffer.position(0);
        GLES20.glEnableVertexAttribArray(positionHandle);
        GLES20.glVertexAttribPointer(
                positionHandle, 2, GLES20.GL_FLOAT, false, 6 * 4, vertexBuffer
        );

        vertexBuffer.position(2);
        GLES20.glEnableVertexAttribArray(colorHandle);
        GLES20.glVertexAttribPointer(
                colorHandle, 4, GLES20.GL_FLOAT, false, 6 * 4, vertexBuffer
        );

        GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3);
        GLES20.glDisableVertexAttribArray(positionHandle);
        GLES20.glDisableVertexAttribArray(colorHandle);
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
}
