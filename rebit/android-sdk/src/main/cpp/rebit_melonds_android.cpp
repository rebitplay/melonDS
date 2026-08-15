#include "rebit_melonds_dual.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <jni.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{

constexpr char LogTag[] = "RebitMelonDS";
constexpr char ExpectedBuild[] = "melonds-dual-parallel-1";
constexpr char ExpectedRuntimeAbi[] = "rebit-melonds-parallel-v1";
constexpr std::size_t MaximumRomBytes = 512U * 1024U * 1024U;
constexpr int MaximumPlayers = 4;
constexpr std::uint32_t ReleasedKeys = 0x0FFF;
constexpr std::size_t AudioCapacityFrames = 48'000;

std::mutex ApiMutex;
std::string AdapterError;

void SetError(std::string message)
{
    AdapterError = std::move(message);
    __android_log_print(ANDROID_LOG_ERROR, LogTag, "%s", AdapterError.c_str());
}

std::string CoreError(const char* fallback)
{
    const char* value = md_last_error();
    return value && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

class UtfString
{
public:
    UtfString(JNIEnv* environment, jstring value)
        : Environment(environment), Value(value), Characters(value ? environment->GetStringUTFChars(value, nullptr) : nullptr)
    {
    }

    ~UtfString()
    {
        if (Characters)
            Environment->ReleaseStringUTFChars(Value, Characters);
    }

    const char* get() const { return Characters; }

private:
    JNIEnv* Environment;
    jstring Value;
    const char* Characters;
};

class AudioOutput
{
public:
    bool EnsureStarted()
    {
        if (Stream && AAudioStream_getState(Stream) != AAUDIO_STREAM_STATE_DISCONNECTED)
            return true;

        Stop();
        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t result = AAudio_createStreamBuilder(&builder);
        if (result != AAUDIO_OK || !builder)
        {
            SetError("Could not create the Android low-latency audio builder.");
            return false;
        }

        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSampleRate(builder, REBIT_MELONDS_DUAL_AUDIO_SAMPLE_RATE);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
        AAudioStreamBuilder_setDataCallback(builder, &AudioOutput::DataCallback, this);
        AAudioStreamBuilder_setErrorCallback(builder, &AudioOutput::ErrorCallback, this);

        result = AAudioStreamBuilder_openStream(builder, &Stream);
        if (result != AAUDIO_OK)
        {
            Stream = nullptr;
            AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
            result = AAudioStreamBuilder_openStream(builder, &Stream);
        }
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK || !Stream)
        {
            SetError(std::string("Could not open Android game audio: ") + AAudio_convertResultToText(result));
            Stream = nullptr;
            return false;
        }
        if (AAudioStream_getSampleRate(Stream) != REBIT_MELONDS_DUAL_AUDIO_SAMPLE_RATE
            || AAudioStream_getChannelCount(Stream) != 2
            || AAudioStream_getFormat(Stream) != AAUDIO_FORMAT_PCM_I16)
        {
            SetError("Android did not provide the required 48 kHz stereo PCM audio stream.");
            Stop();
            return false;
        }

        const int32_t burst = std::max(1, AAudioStream_getFramesPerBurst(Stream));
        AAudioStream_setBufferSizeInFrames(Stream, burst * 2);
        result = AAudioStream_requestStart(Stream);
        if (result != AAUDIO_OK)
        {
            SetError(std::string("Could not start Android game audio: ") + AAudio_convertResultToText(result));
            Stop();
            return false;
        }
        Running.store(true, std::memory_order_release);
        return true;
    }

    void Pause()
    {
        if (Stream)
            AAudioStream_requestPause(Stream);
        Running.store(false, std::memory_order_release);
    }

    void Stop()
    {
        Running.store(false, std::memory_order_release);
        if (Stream)
        {
            AAudioStream_requestStop(Stream);
            AAudioStream_close(Stream);
            Stream = nullptr;
        }
        ReadFrame.store(0, std::memory_order_release);
        WriteFrame.store(0, std::memory_order_release);
    }

    void SetVolume(float volume)
    {
        Volume.store(std::clamp(volume, 0.0F, 1.0F), std::memory_order_release);
    }

    void PumpCoreAudio()
    {
        int available = std::max(0, md_audio_available());
        while (available > 0)
        {
            const int requested = std::min(available, 4096);
            const int frames = md_audio_read(requested);
            const std::int16_t* samples = md_audio_buffer();
            if (frames <= 0 || !samples)
                break;
            Push(samples, static_cast<std::size_t>(frames));
            available -= frames;
        }
    }

    std::array<std::uint64_t, 4> Stats() const
    {
        const std::uint64_t read = ReadFrame.load(std::memory_order_acquire);
        const std::uint64_t write = WriteFrame.load(std::memory_order_acquire);
        return {
            Running.load(std::memory_order_acquire) ? 1U : 0U,
            write >= read ? std::min<std::uint64_t>(write - read, AudioCapacityFrames) : 0U,
            Underflows.load(std::memory_order_relaxed),
            DroppedFrames.load(std::memory_order_relaxed),
        };
    }

private:
    static aaudio_data_callback_result_t DataCallback(
        AAudioStream*,
        void* userdata,
        void* audioData,
        int32_t numberFrames)
    {
        return static_cast<AudioOutput*>(userdata)->Fill(static_cast<std::int16_t*>(audioData), numberFrames);
    }

    static void ErrorCallback(AAudioStream*, void* userdata, aaudio_result_t error)
    {
        auto* self = static_cast<AudioOutput*>(userdata);
        self->Running.store(false, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "AAudio disconnected: %s", AAudio_convertResultToText(error));
    }

    aaudio_data_callback_result_t Fill(std::int16_t* output, int32_t requestedFrames)
    {
        if (!output || requestedFrames <= 0)
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        const std::uint64_t read = ReadFrame.load(std::memory_order_relaxed);
        const std::uint64_t write = WriteFrame.load(std::memory_order_acquire);
        const std::size_t available = write >= read
            ? static_cast<std::size_t>(std::min<std::uint64_t>(write - read, AudioCapacityFrames))
            : 0;
        const std::size_t consumed = std::min<std::size_t>(available, requestedFrames);
        const float gain = Volume.load(std::memory_order_acquire);

        for (std::size_t frame = 0; frame < consumed; ++frame)
        {
            const std::size_t ringFrame = static_cast<std::size_t>((read + frame) % AudioCapacityFrames);
            for (int channel = 0; channel < 2; ++channel)
            {
                const int sample = static_cast<int>(static_cast<float>(Ring[ringFrame * 2 + channel]) * gain);
                output[frame * 2 + channel] = static_cast<std::int16_t>(std::clamp(sample, -32768, 32767));
            }
        }
        if (consumed < static_cast<std::size_t>(requestedFrames))
        {
            std::memset(
                output + consumed * 2,
                0,
                (static_cast<std::size_t>(requestedFrames) - consumed) * 2 * sizeof(std::int16_t));
            Underflows.fetch_add(1, std::memory_order_relaxed);
        }
        ReadFrame.store(read + consumed, std::memory_order_release);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    void Push(const std::int16_t* samples, std::size_t frames)
    {
        if (!samples || frames == 0)
            return;
        const std::uint64_t write = WriteFrame.load(std::memory_order_relaxed);
        const std::uint64_t read = ReadFrame.load(std::memory_order_acquire);
        const std::size_t buffered = write >= read
            ? static_cast<std::size_t>(std::min<std::uint64_t>(write - read, AudioCapacityFrames))
            : AudioCapacityFrames;
        const std::size_t accepted = std::min(frames, AudioCapacityFrames - buffered);
        for (std::size_t frame = 0; frame < accepted; ++frame)
        {
            const std::size_t ringFrame = static_cast<std::size_t>((write + frame) % AudioCapacityFrames);
            Ring[ringFrame * 2] = samples[frame * 2];
            Ring[ringFrame * 2 + 1] = samples[frame * 2 + 1];
        }
        WriteFrame.store(write + accepted, std::memory_order_release);
        if (accepted < frames)
            DroppedFrames.fetch_add(frames - accepted, std::memory_order_relaxed);
    }

    AAudioStream* Stream = nullptr;
    std::array<std::int16_t, AudioCapacityFrames * 2> Ring {};
    std::atomic<std::uint64_t> ReadFrame {0};
    std::atomic<std::uint64_t> WriteFrame {0};
    std::atomic<std::uint64_t> Underflows {0};
    std::atomic<std::uint64_t> DroppedFrames {0};
    std::atomic<float> Volume {1.0F};
    std::atomic<bool> Running {false};
};

class VideoOutput
{
public:
    void SetWindow(ANativeWindow* window)
    {
        DestroySurface();
        if (Window)
            ANativeWindow_release(Window);
        Window = window;
    }

    bool Render(const std::uint32_t* pixels)
    {
        if (!Window || !pixels)
            return true;
        if (!EnsureSurface())
            return false;
        if (!eglMakeCurrent(Display, Surface, Surface, Context))
        {
            SetError("Could not activate the Android NDS video surface.");
            return false;
        }

        EGLint width = 0;
        EGLint height = 0;
        eglQuerySurface(Display, Surface, EGL_WIDTH, &width);
        eglQuerySurface(Display, Surface, EGL_HEIGHT, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(Program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, Texture);
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            REBIT_MELONDS_DUAL_WIDTH,
            REBIT_MELONDS_DUAL_HEIGHT,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            pixels);
        glBindBuffer(GL_ARRAY_BUFFER, VertexBuffer);
        glEnableVertexAttribArray(PositionLocation);
        glEnableVertexAttribArray(TextureLocation);
        glVertexAttribPointer(PositionLocation, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, nullptr);
        glVertexAttribPointer(
            TextureLocation,
            2,
            GL_FLOAT,
            GL_FALSE,
            sizeof(float) * 4,
            reinterpret_cast<const void*>(sizeof(float) * 2));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (!eglSwapBuffers(Display, Surface))
        {
            SetError("Could not present the Android NDS video frame.");
            return false;
        }
        return true;
    }

    void Reset()
    {
        DestroySurface();
        if (Display != EGL_NO_DISPLAY && Context != EGL_NO_CONTEXT)
        {
            eglMakeCurrent(Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(Display, Context);
        }
        if (Display != EGL_NO_DISPLAY)
            eglTerminate(Display);
        Display = EGL_NO_DISPLAY;
        Context = EGL_NO_CONTEXT;
        Config = nullptr;
        Program = 0;
        Texture = 0;
        VertexBuffer = 0;
        if (Window)
            ANativeWindow_release(Window);
        Window = nullptr;
    }

private:
    static GLuint CompileShader(GLenum type, const char* source)
    {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_TRUE)
            return shader;
        char log[512] {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "Shader compilation failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }

    bool EnsureDisplay()
    {
        if (Display != EGL_NO_DISPLAY)
            return true;
        Display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (Display == EGL_NO_DISPLAY || !eglInitialize(Display, nullptr, nullptr))
        {
            SetError("Could not initialize Android EGL for NDS video.");
            Display = EGL_NO_DISPLAY;
            return false;
        }
        const EGLint attributes[] = {
            EGL_RENDERABLE_TYPE,
            EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE,
            EGL_WINDOW_BIT,
            EGL_RED_SIZE,
            8,
            EGL_GREEN_SIZE,
            8,
            EGL_BLUE_SIZE,
            8,
            EGL_ALPHA_SIZE,
            8,
            EGL_NONE,
        };
        EGLint configurations = 0;
        if (!eglChooseConfig(Display, attributes, &Config, 1, &configurations) || configurations != 1)
        {
            SetError("Could not choose an Android EGL video configuration.");
            return false;
        }
        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        Context = eglCreateContext(Display, Config, EGL_NO_CONTEXT, contextAttributes);
        if (Context == EGL_NO_CONTEXT)
        {
            SetError("Could not create the Android NDS OpenGL context.");
            return false;
        }
        return true;
    }

    bool EnsureSurface()
    {
        if (Surface != EGL_NO_SURFACE)
            return true;
        if (!EnsureDisplay() || !Window)
            return false;
        Surface = eglCreateWindowSurface(Display, Config, Window, nullptr);
        if (Surface == EGL_NO_SURFACE || !eglMakeCurrent(Display, Surface, Surface, Context))
        {
            SetError("Could not create the Android NDS window surface.");
            Surface = EGL_NO_SURFACE;
            return false;
        }
        eglSwapInterval(Display, 0);
        return EnsureObjects();
    }

    bool EnsureObjects()
    {
        if (Program)
            return true;
        constexpr char VertexShader[] =
            "attribute vec2 position;"
            "attribute vec2 textureCoordinate;"
            "varying vec2 texturePosition;"
            "void main(){gl_Position=vec4(position,0.0,1.0);texturePosition=textureCoordinate;}";
        constexpr char FragmentShader[] =
            "precision mediump float;"
            "uniform sampler2D frameTexture;"
            "varying vec2 texturePosition;"
            "void main(){gl_FragColor=texture2D(frameTexture,texturePosition);}";
        const GLuint vertex = CompileShader(GL_VERTEX_SHADER, VertexShader);
        const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, FragmentShader);
        if (!vertex || !fragment)
        {
            if (vertex)
                glDeleteShader(vertex);
            if (fragment)
                glDeleteShader(fragment);
            SetError("Could not compile the Android NDS video shaders.");
            return false;
        }
        Program = glCreateProgram();
        glAttachShader(Program, vertex);
        glAttachShader(Program, fragment);
        glLinkProgram(Program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = GL_FALSE;
        glGetProgramiv(Program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE)
        {
            SetError("Could not link the Android NDS video shaders.");
            return false;
        }

        PositionLocation = glGetAttribLocation(Program, "position");
        TextureLocation = glGetAttribLocation(Program, "textureCoordinate");
        const GLint sampler = glGetUniformLocation(Program, "frameTexture");
        glUseProgram(Program);
        glUniform1i(sampler, 0);

        constexpr float vertices[] = {
            -1.0F, 1.0F, 0.0F, 0.0F,
            -1.0F, -1.0F, 0.0F, 1.0F,
            1.0F, 1.0F, 1.0F, 0.0F,
            1.0F, -1.0F, 1.0F, 1.0F,
        };
        glGenBuffers(1, &VertexBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, VertexBuffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        glGenTextures(1, &Texture);
        glBindTexture(GL_TEXTURE_2D, Texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            REBIT_MELONDS_DUAL_WIDTH,
            REBIT_MELONDS_DUAL_HEIGHT,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            nullptr);
        return glGetError() == GL_NO_ERROR;
    }

    void DestroySurface()
    {
        if (Display != EGL_NO_DISPLAY && Surface != EGL_NO_SURFACE)
        {
            eglMakeCurrent(Display, EGL_NO_SURFACE, EGL_NO_SURFACE, Context);
            eglDestroySurface(Display, Surface);
        }
        Surface = EGL_NO_SURFACE;
    }

    ANativeWindow* Window = nullptr;
    EGLDisplay Display = EGL_NO_DISPLAY;
    EGLContext Context = EGL_NO_CONTEXT;
    EGLSurface Surface = EGL_NO_SURFACE;
    EGLConfig Config = nullptr;
    GLuint Program = 0;
    GLuint Texture = 0;
    GLuint VertexBuffer = 0;
    GLint PositionLocation = -1;
    GLint TextureLocation = -1;
};

AudioOutput Audio;
VideoOutput Video;

bool RuntimeIdentityMatches()
{
    return md_api_version() == REBIT_MELONDS_DUAL_API_VERSION
        && std::strcmp(md_build_id(), ExpectedBuild) == 0
        && std::strcmp(md_runtime_abi(), ExpectedRuntimeAbi) == 0;
}

std::vector<std::uint8_t> ReadRom(const char* path)
{
    if (!path || path[0] == '\0')
        return {};
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return {};
    const std::streamoff length = stream.tellg();
    if (length < 4096 || static_cast<std::uint64_t>(length) > MaximumRomBytes)
        return {};
    stream.seekg(0);
    std::vector<std::uint8_t> rom(static_cast<std::size_t>(length));
    if (!stream.read(reinterpret_cast<char*>(rom.data()), length))
        return {};
    return rom;
}

}

extern "C"
{

JNIEXPORT jstring JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeIdentity(JNIEnv* environment, jclass)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    std::ostringstream value;
    value << md_api_version() << '|' << md_build_id() << '|' << md_runtime_abi();
    return environment->NewStringUTF(value.str().c_str());
}

JNIEXPORT jboolean JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeLoad(
    JNIEnv* environment,
    jclass,
    jstring romPath,
    jint players,
    jlong seed)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    AdapterError.clear();
    Audio.Stop();
    md_destroy();
    if (!RuntimeIdentityMatches())
    {
        SetError("The bundled native melonDS engine identity is invalid.");
        return JNI_FALSE;
    }
    UtfString path(environment, romPath);
    std::vector<std::uint8_t> rom = ReadRom(path.get());
    if (rom.empty())
    {
        SetError("Could not read the verified Nintendo DS ROM from native storage.");
        return JNI_FALSE;
    }
    const std::uint64_t seedBits = static_cast<std::uint64_t>(seed);
    if (!md_load(
            rom.data(),
            static_cast<std::uint32_t>(rom.size()),
            players,
            static_cast<std::uint32_t>(seedBits),
            static_cast<std::uint32_t>(seedBits >> 32)))
    {
        SetError(CoreError("The native melonDS engine rejected the Nintendo DS ROM."));
        return JNI_FALSE;
    }
    if (!Video.Render(md_framebuffer(md_visible_player())))
    {
        md_destroy();
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeDestroy(JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    Audio.Stop();
    Video.Reset();
    md_destroy();
    AdapterError.clear();
}

JNIEXPORT jstring JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeLastError(JNIEnv* environment, jclass)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    const std::string message = AdapterError.empty() ? CoreError("Native melonDS failed.") : AdapterError;
    return environment->NewStringUTF(message.c_str());
}

JNIEXPORT jboolean JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeSetVisiblePlayer(JNIEnv*, jclass, jint player)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    if (player < 0 || player >= md_player_count())
        return JNI_FALSE;
    md_set_visible_player(player);
    return Video.Render(md_framebuffer(player)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeRunFrame(
    JNIEnv* environment,
    jclass,
    jintArray keysArray,
    jbooleanArray touchingArray,
    jintArray touchXArray,
    jintArray touchYArray)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    const int players = md_player_count();
    if (!md_is_loaded()
        || players < 2
        || players > MaximumPlayers
        || !keysArray
        || !touchingArray
        || !touchXArray
        || !touchYArray
        || environment->GetArrayLength(keysArray) != players
        || environment->GetArrayLength(touchingArray) != players
        || environment->GetArrayLength(touchXArray) != players
        || environment->GetArrayLength(touchYArray) != players)
    {
        SetError("The native NDS frame input array is invalid.");
        return JNI_FALSE;
    }

    std::array<jint, MaximumPlayers> keys {};
    std::array<jboolean, MaximumPlayers> touching {};
    std::array<jint, MaximumPlayers> touchX {};
    std::array<jint, MaximumPlayers> touchY {};
    environment->GetIntArrayRegion(keysArray, 0, players, keys.data());
    environment->GetBooleanArrayRegion(touchingArray, 0, players, touching.data());
    environment->GetIntArrayRegion(touchXArray, 0, players, touchX.data());
    environment->GetIntArrayRegion(touchYArray, 0, players, touchY.data());
    if (environment->ExceptionCheck())
        return JNI_FALSE;

    for (int player = 0; player < players; ++player)
    {
        if (!md_set_input(
                player,
                static_cast<std::uint32_t>(keys[player]) & ReleasedKeys,
                touching[player] == JNI_TRUE ? 1 : 0,
                touchX[player],
                touchY[player]))
        {
            SetError("The native melonDS engine rejected synchronized input.");
            return JNI_FALSE;
        }
    }
    if (!md_run_frame())
    {
        SetError(CoreError("The native melonDS engine could not advance a synchronized frame."));
        return JNI_FALSE;
    }
    if (!Video.Render(md_framebuffer(md_visible_player())))
        return JNI_FALSE;
    Audio.PumpCoreAudio();
    if (!Audio.EnsureStarted())
        return JNI_FALSE;
    return JNI_TRUE;
}

JNIEXPORT jlong JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeFrame(JNIEnv*, jclass, jint player)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    return static_cast<jlong>(md_frame(player));
}

JNIEXPORT jlong JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeStateHash(JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    return static_cast<jlong>(md_state_hash());
}

JNIEXPORT jdouble JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeLastFrameMilliseconds(JNIEnv*, jclass)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    return md_last_frame_ms();
}

JNIEXPORT jlongArray JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativePlayerCounters(JNIEnv* environment, jclass, jint player)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    const auto audio = Audio.Stats();
    const std::array<jlong, 8> counters = {
        static_cast<jlong>(md_mp_packets_sent(player)),
        static_cast<jlong>(md_mp_packets_received(player)),
        static_cast<jlong>(md_mp_commands(player)),
        static_cast<jlong>(md_mp_replies(player)),
        static_cast<jlong>(audio[0]),
        static_cast<jlong>(audio[1]),
        static_cast<jlong>(audio[2]),
        static_cast<jlong>(audio[3]),
    };
    jlongArray result = environment->NewLongArray(counters.size());
    if (result)
        environment->SetLongArrayRegion(result, 0, counters.size(), counters.data());
    return result;
}

JNIEXPORT jbyteArray JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeExportSave(JNIEnv* environment, jclass, jint player)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    const std::uint32_t length = md_save_size(player);
    const std::uint8_t* bytes = md_save_data(player);
    if (!bytes || length == 0 || length > static_cast<std::uint32_t>(std::numeric_limits<jsize>::max()))
        return nullptr;
    jbyteArray result = environment->NewByteArray(static_cast<jsize>(length));
    if (result)
        environment->SetByteArrayRegion(result, 0, static_cast<jsize>(length), reinterpret_cast<const jbyte*>(bytes));
    return result;
}

JNIEXPORT jboolean JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeImportSave(
    JNIEnv* environment,
    jclass,
    jint player,
    jbyteArray save)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    if (!save)
        return JNI_FALSE;
    const jsize length = environment->GetArrayLength(save);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    environment->GetByteArrayRegion(save, 0, length, reinterpret_cast<jbyte*>(bytes.data()));
    return md_import_save(player, bytes.data(), static_cast<std::uint32_t>(bytes.size())) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jbyteArray JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeExportCheckpoint(JNIEnv* environment, jclass)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    if (!md_export_checkpoint())
    {
        SetError(CoreError("The native melonDS engine could not export a checkpoint."));
        return nullptr;
    }
    const std::uint32_t length = md_checkpoint_size();
    const std::uint8_t* bytes = md_checkpoint_data();
    if (!bytes || length == 0 || length > static_cast<std::uint32_t>(std::numeric_limits<jsize>::max()))
    {
        md_clear_checkpoint();
        SetError("The native melonDS engine returned an invalid checkpoint.");
        return nullptr;
    }
    jbyteArray result = environment->NewByteArray(static_cast<jsize>(length));
    if (result)
        environment->SetByteArrayRegion(result, 0, static_cast<jsize>(length), reinterpret_cast<const jbyte*>(bytes));
    md_clear_checkpoint();
    return result;
}

JNIEXPORT jboolean JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeImportCheckpoint(
    JNIEnv* environment,
    jclass,
    jbyteArray checkpoint)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    if (!checkpoint)
        return JNI_FALSE;
    const jsize length = environment->GetArrayLength(checkpoint);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    environment->GetByteArrayRegion(checkpoint, 0, length, reinterpret_cast<jbyte*>(bytes.data()));
    if (!md_import_checkpoint(bytes.data(), static_cast<std::uint32_t>(bytes.size())))
    {
        SetError(CoreError("The native melonDS engine rejected the checkpoint."));
        return JNI_FALSE;
    }
    return Video.Render(md_framebuffer(md_visible_player())) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeInjectDesync(JNIEnv*, jclass, jint player)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    return md_inject_desync_for_test(player, std::numeric_limits<std::uint32_t>::max(), 0x5A) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jintArray JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeCaptureArgb(JNIEnv* environment, jclass)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    const std::uint32_t* source = md_framebuffer(md_visible_player());
    if (!source)
        return nullptr;
    constexpr std::size_t pixels = REBIT_MELONDS_DUAL_WIDTH * REBIT_MELONDS_DUAL_HEIGHT;
    std::vector<jint> argb(pixels);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        const std::uint32_t rgba = source[index];
        argb[index] = static_cast<jint>(
            0xFF000000U | ((rgba & 0x000000FFU) << 16) | (rgba & 0x0000FF00U) | ((rgba >> 16) & 0xFFU));
    }
    jintArray result = environment->NewIntArray(static_cast<jsize>(pixels));
    if (result)
        environment->SetIntArrayRegion(result, 0, static_cast<jsize>(pixels), argb.data());
    return result;
}

JNIEXPORT void JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeSetSurface(JNIEnv* environment, jclass, jobject surface)
{
    std::lock_guard<std::mutex> guard(ApiMutex);
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(environment, surface) : nullptr;
    Video.SetWindow(window);
    if (window && md_is_loaded())
        Video.Render(md_framebuffer(md_visible_player()));
}

JNIEXPORT void JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeSetVolume(JNIEnv*, jclass, jfloat volume)
{
    Audio.SetVolume(volume);
}

JNIEXPORT jboolean JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativeStartAudio(JNIEnv*, jclass)
{
    return Audio.EnsureStarted() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_cc_rebit_melonds_RebitMelonDSNative_nativePauseAudio(JNIEnv*, jclass)
{
    Audio.Pause();
}

}
