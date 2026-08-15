package cc.rebit.melonds;

import android.view.Surface;

public final class RebitMelonDSNative {
    public static final int API_VERSION = 1;
    public static final String ENGINE_BUILD = "melonds-dual-parallel-1";
    public static final String RUNTIME_ABI = "rebit-melonds-parallel-v1";
    public static final int WIDTH = 256;
    public static final int HEIGHT = 384;
    public static final int AUDIO_SAMPLE_RATE = 48_000;

    static {
        System.loadLibrary("rebit_melonds_android");
    }

    private RebitMelonDSNative() {}

    public static native String nativeIdentity();

    public static native boolean nativeLoad(String romPath, int players, long seed);

    public static native void nativeDestroy();

    public static native String nativeLastError();

    public static native boolean nativeSetVisiblePlayer(int player);

    public static native boolean nativeRunFrame(int[] keys, boolean[] touching, int[] touchX, int[] touchY);

    public static native long nativeFrame(int player);

    public static native long nativeStateHash();

    public static native double nativeLastFrameMilliseconds();

    public static native long[] nativePlayerCounters(int player);

    public static native byte[] nativeExportSave(int player);

    public static native boolean nativeImportSave(int player, byte[] save);

    public static native byte[] nativeExportCheckpoint();

    public static native boolean nativeImportCheckpoint(byte[] checkpoint);

    public static native boolean nativeInjectDesync(int player);

    public static native int[] nativeCaptureArgb();

    public static native void nativeSetSurface(Surface surface);

    public static native void nativeSetVolume(float volume);

    public static native boolean nativeStartAudio();

    public static native void nativePauseAudio();
}
