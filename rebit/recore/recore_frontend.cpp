// A small libretro frontend for melonDS 1.x, made for Recore (single player, single thread).
//
// It does what the Recore single-player core needs: the game from memory, BIOS and firmware from
// the system directory (FreeBIOS and direct boot otherwise), XRGB8888 screens in a few layouts,
// touch through the pointer, 32768 Hz audio, the in-game save as /saves/<rom>.sav, states, reset
// and the Recore `mic` extension. No OpenGL, WFC, cheats or DSi yet.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "libretro.h"
#include "rebit_mic.h"
#include "frontend_internal.h"

#include "Args.h"
#include "GPU.h"
#include "GPU_Soft.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Platform.h"
#include "SPI.h"
#include "SPI_Firmware.h"
#include "Savestate.h"
#include "version.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace melonDS { extern u32 RebitJitNativeMask; } // diagnostics, ARMJIT_WASM
using namespace melonDS;

namespace
{
retro_environment_t environ_cb;
retro_video_refresh_t video_cb;
retro_audio_sample_batch_t audio_batch_cb;
retro_input_poll_t input_poll_cb;
retro_input_state_t input_state_cb;
retro_log_printf_t log_cb;

constexpr int ScreenW = 256;
constexpr int ScreenH = 192;

enum class Layout { TopBottom, BottomTop, LeftRight, RightLeft, TopOnly, BottomOnly };

struct LayoutInfo
{
    int width, height;
    int topX, topY;       // -1: not shown
    int bottomX, bottomY; // -1: not shown
};

std::unique_ptr<NDS> nds;
std::vector<u8> romData;
std::string romName;
std::string savePath;

Layout layout = Layout::TopBottom;
bool touchEnabled = true;
bool jitEnabled = true;
bool jitStats = false;
bool jitBranchOptimizations = true;
bool jitLiteralOptimizations = true;
int jitMaxBlockSize = 32;
u64 framesRun = 0;
bool touching = false;
u32 frameBuffer[ScreenW * 2 * ScreenH * 2];

// in-game save: melonDS hands over the whole save on every write; it reaches the file once the game
// has stopped writing for a while, as the 0.9.3 frontend did
std::vector<u8> pendingSave;
bool saveDirty = false;
u64 saveTouchedMs = 0;
constexpr u64 SaveFlushDelayMs = 2000;

RebitMicBuffer rebitMic;
bool micOpen = false;
double micPhase = 0;


LayoutInfo CurrentLayout()
{
    switch (layout)
    {
    case Layout::BottomTop: return {ScreenW, ScreenH * 2, 0, ScreenH, 0, 0};
    case Layout::LeftRight: return {ScreenW * 2, ScreenH, 0, 0, ScreenW, 0};
    case Layout::RightLeft: return {ScreenW * 2, ScreenH, ScreenW, 0, 0, 0};
    case Layout::TopOnly: return {ScreenW, ScreenH, 0, 0, -1, -1};
    case Layout::BottomOnly: return {ScreenW, ScreenH, -1, -1, 0, 0};
    case Layout::TopBottom:
    default: return {ScreenW, ScreenH * 2, 0, 0, 0, ScreenH};
    }
}

const char* GetVariable(const char* key)
{
    retro_variable var = {key, nullptr};
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        return var.value;
    return nullptr;
}

void ReadVariables()
{
    if (const char* value = GetVariable("melonds_screen_layout"))
    {
        if (!strcmp(value, "Bottom/Top")) layout = Layout::BottomTop;
        else if (!strcmp(value, "Left/Right")) layout = Layout::LeftRight;
        else if (!strcmp(value, "Right/Left")) layout = Layout::RightLeft;
        else if (!strcmp(value, "Top Only")) layout = Layout::TopOnly;
        else if (!strcmp(value, "Bottom Only")) layout = Layout::BottomOnly;
        else layout = Layout::TopBottom;
    }
    if (const char* value = GetVariable("melonds_touch_mode"))
        touchEnabled = strcmp(value, "Disabled") != 0;
    if (const char* value = GetVariable("melonds_jit"))
        jitEnabled = strcmp(value, "disabled") != 0;
    if (const char* value = GetVariable("melonds_jit_stats"))
        jitStats = strcmp(value, "enabled") == 0;
    if (const char* value = GetVariable("melonds_jit_branch"))
        jitBranchOptimizations = strcmp(value, "disabled") != 0;
    if (const char* value = GetVariable("melonds_jit_literal"))
        jitLiteralOptimizations = strcmp(value, "disabled") != 0;
    if (const char* value = GetVariable("melonds_jit_native"))
        melonDS::RebitJitNativeMask = strtoul(value, nullptr, 0);
    if (const char* value = GetVariable("melonds_jit_block_size"))
        jitMaxBlockSize = atoi(value) > 0 ? atoi(value) : 32;
}

const char* SystemDirectory()
{
    const char* dir = nullptr;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
        return dir;
    return ".";
}

const char* SaveDirectory()
{
    const char* dir = nullptr;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
        return dir;
    return SystemDirectory();
}

bool ReadFile(const std::string& path, std::vector<u8>& out)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    out.resize(length > 0 ? length : 0);
    const bool ok = length <= 0 || std::fread(out.data(), 1, out.size(), file) == out.size();
    std::fclose(file);
    return ok;
}

void FlushSave(bool force)
{
    if (!saveDirty || savePath.empty())
        return;
    if (!force && Platform::GetMSCount() - saveTouchedMs < SaveFlushDelayMs)
        return;
    if (std::FILE* file = std::fopen(savePath.c_str(), "wb"))
    {
        std::fwrite(pendingSave.data(), 1, pendingSave.size(), file);
        std::fclose(file);
    }
    saveDirty = false;
}

void SetDateTimeFromHost()
{
    const std::time_t now = std::time(nullptr);
    std::tm local = *std::localtime(&now);
    nds->RTC.SetDateTime(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}

void BootCart()
{
    nds->Reset();
    if (nds->NeedsDirectBoot())
        nds->SetupDirectBoot(romName);
    SetDateTimeFromHost();
    nds->Start();
}

void UpdateInput()
{
    input_poll_cb();
    static const unsigned keys[12] = {
        RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
        RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
        RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_Y,
    };
    const u32 bits = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
    u32 mask = 0xFFF;
    for (int i = 0; i < 12; i++)
        if (bits & (1u << keys[i]))
            mask &= ~(1u << i);
    nds->SetKeyMask(mask);

    const bool lidClosed = bits & (1u << RETRO_DEVICE_ID_JOYPAD_L3);
    if (lidClosed != nds->IsLidClosed())
        nds->SetLidClosed(lidClosed);

    bool touchNow = false;
    const LayoutInfo info = CurrentLayout();
    if (touchEnabled && info.bottomX >= 0 && input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED))
    {
        const int px = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
        const int py = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
        const int x = (px + 0x8000) * info.width / 0x10000 - info.bottomX;
        const int y = (py + 0x8000) * info.height / 0x10000 - info.bottomY;
        if (x >= 0 && x < ScreenW && y >= 0 && y < ScreenH)
        {
            nds->TouchScreen(x, y);
            touchNow = true;
        }
    }
    if (!touchNow && touching)
        nds->ReleaseScreen();
    touching = touchNow;
}

void Present()
{
    void* top = nullptr;
    void* bottom = nullptr;
    if (!nds->GPU.GetRenderer().GetFramebuffers(&top, &bottom) || !top || !bottom)
        return;
    const LayoutInfo info = CurrentLayout();
    auto blit = [&](const void* source, int x0, int y0) {
        if (x0 < 0)
            return;
        const u32* src = static_cast<const u32*>(source);
        for (int y = 0; y < ScreenH; y++)
            memcpy(&frameBuffer[(y0 + y) * info.width + x0], &src[y * ScreenW], ScreenW * sizeof(u32));
    };
    blit(top, info.topX, info.topY);
    blit(bottom, info.bottomX, info.bottomY);
    video_cb(frameBuffer, info.width, info.height, info.width * sizeof(u32));
}

void DrainAudio()
{
    static s16 buffer[0x1000 * 2];
    for (;;)
    {
        int available = nds->SPU.GetOutputSize();
        if (available <= 0)
            break;
        if (available > 0x1000)
            available = 0x1000;
        const int read = nds->SPU.ReadOutput(buffer, available);
        if (read <= 0)
            break;
        audio_batch_cb(buffer, read);
    }
}

void Log(retro_log_level level, const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    // stderr as well: Recore's host shows the module's output, as the 0.9.3 core's printf did
    if (log_cb)
        log_cb(level, "%s", line);
    std::fputs(line, stderr);
}
}

namespace rebit
{
void FrontendLog(int level, const char* line)
{
    Log(level >= 3 ? RETRO_LOG_ERROR : level == 2 ? RETRO_LOG_WARN : RETRO_LOG_INFO, "%s", line);
}

void StoreNDSSave(const u8* data, u32 length)
{
    pendingSave.assign(data, data + length);
    saveDirty = true;
    saveTouchedMs = Platform::GetMSCount();
}

void MicOpened(bool open)
{
    micOpen = open;
}

// The DS microphone reads at ~47.6 kHz; the host pushes 44.1 kHz mono. Nearest-sample resampling.
int MicRead(s16* data, int maxlength)
{
    if (!micOpen || !rebitMic.is_active() || maxlength <= 0)
        return 0;
    constexpr double Step = 44100.0 / 47605.0;
    static s16 source[RebitMicBuffer::Capacity];
    const int wanted = (int)(maxlength * Step) + 1;
    const int got = rebitMic.read(source, wanted < RebitMicBuffer::Capacity ? wanted : RebitMicBuffer::Capacity);
    if (got <= 0)
        return 0;
    int out = 0;
    while (out < maxlength)
    {
        const int index = (int)micPhase;
        if (index >= got)
            break;
        data[out++] = source[index];
        micPhase += Step;
    }
    micPhase -= got;
    if (micPhase < 0)
        micPhase = 0;
    return out;
}
}

extern "C" EMSCRIPTEN_KEEPALIVE void rebit_mic_push(const int16_t* samples, int count)
{
    rebitMic.push(samples, count);
}

extern "C" EMSCRIPTEN_KEEPALIVE void rebit_mic_set_active(int on)
{
    rebitMic.set_active(on != 0);
    micPhase = 0;
    // the DS hears silence from now on, not what melonDS had buffered
    if (!on && nds)
        nds->Mic.ClearInput();
}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;
    static const retro_variable variables[] = {
        {"melonds_screen_layout", "Screen layout; Top/Bottom|Bottom/Top|Left/Right|Right/Left|Top Only|Bottom Only"},
        {"melonds_touch_mode", "Touch mode; Touch|Disabled"},
        {"melonds_jit", "JIT (ARM code compiled to WebAssembly, read at load); enabled|disabled"},
        {"melonds_jit_stats", "JIT statistics in the log every 60 frames (diagnostics); disabled|enabled"},
        {"melonds_jit_branch", "JIT branch following (diagnostics); enabled|disabled"},
        {"melonds_jit_literal", "JIT literal optimisation (diagnostics); enabled|disabled"},
        {"melonds_jit_block_size", "JIT maximum block size (diagnostics); 32|1|2|4|8|16"},
        {"melonds_jit_native", "JIT native groups (diagnostics: 1 ARM ALU, 2 ARM branch, 4 Thumb ALU, 8 Thumb branch, 0x10000 idle loops); 0xffffffff|0|0x1|0x2|0x3|0x4|0x5|0x6|0x7|0x8|0x9|0xa|0xb|0xc|0xd|0xe|0xf|0x10000|0x10001|0x10002|0x10003|0x10004|0x10005|0x10006|0x10007|0x10008|0x10009|0x1000a|0x1000b|0x1000c|0x1000d|0x1000e|0x1000f"},
        {"melonds_opengl_renderer", "OpenGL renderer (not yet in the 1.x core); disabled"},
        {"melonds_wfc_online", "Nintendo WFC online (not yet in the 1.x core); disabled"},
        {nullptr, nullptr},
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)variables);
    retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t) {}
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_init(void) {}

RETRO_API void retro_deinit(void)
{
    nds.reset();
}

RETRO_API void retro_get_system_info(retro_system_info* info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "melonDS";
    info->library_version = MELONDS_VERSION;
    info->need_fullpath = false;
    info->valid_extensions = "nds";
}

RETRO_API void retro_get_system_av_info(retro_system_av_info* info)
{
    const LayoutInfo layoutInfo = CurrentLayout();
    memset(info, 0, sizeof(*info));
    info->timing.fps = 32.0 * 1024.0 * 1024.0 / 560190.0;
    info->timing.sample_rate = 32.0 * 1024.0;
    info->geometry.base_width = layoutInfo.width;
    info->geometry.base_height = layoutInfo.height;
    info->geometry.max_width = ScreenW * 2;
    info->geometry.max_height = ScreenH * 2;
    info->geometry.aspect_ratio = (float)layoutInfo.width / layoutInfo.height;
}

RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}

RETRO_API void retro_reset(void)
{
    if (!nds)
        return;
    FlushSave(true);
    BootCart();
}

RETRO_API void retro_run(void)
{
    if (!nds)
        return;
    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
    {
        const Layout before = layout;
        ReadVariables();
        if (layout != before)
        {
            retro_system_av_info av;
            retro_get_system_av_info(&av);
            environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av.geometry);
        }
    }

    UpdateInput();
    nds->RunFrame();
    framesRun++;
#ifdef JIT_ENABLED
    if (jitStats && framesRun % 60 == 0)
    {
        auto& compiler = nds->JIT.JITCompiler;
        Log(RETRO_LOG_INFO, "JIT: frame %llu, %u blocks compiled, %llu bytes, %u ms compiling\n",
            (unsigned long long)framesRun, compiler.BlocksCompiled, (unsigned long long)compiler.BytesEmitted,
            (unsigned)(compiler.CompileMicroseconds / 1000));
    }
#endif
    Present();
    DrainAudio();
    FlushSave(false);
}

RETRO_API bool retro_load_game(const retro_game_info* game)
{
    if (!game || !game->data || !game->size)
        return false;

    retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format))
        return false;
    ReadVariables();

    // the name of the save: the content's base name
    std::string base = game->path ? game->path : "game.nds";
    if (const size_t slash = base.find_last_of('/'); slash != std::string::npos)
        base = base.substr(slash + 1);
    romName = base;
    if (const size_t dot = base.find_last_of('.'); dot != std::string::npos)
        base = base.substr(0, dot);
    savePath = std::string(SaveDirectory()) + "/" + base + ".sav";

    NDSArgs args;
    // melonDS's JIT with the WebAssembly back end. No fast memory (it needs memory mapping) and,
    // with branch following and idle loop detection.
    if (jitEnabled)
        args.JIT = JITArgs {(unsigned)jitMaxBlockSize, jitLiteralOptimizations, jitBranchOptimizations, false};
    else
        args.JIT = std::nullopt;
    args.OutputSampleRate = 32.0 * 1024.0;

    const std::string system = SystemDirectory();
    std::vector<u8> file;
    if (ReadFile(system + "/bios9.bin", file) && file.size() == ARM9BIOSSize)
        memcpy(args.ARM9BIOS->data(), file.data(), ARM9BIOSSize);
    else
        Log(RETRO_LOG_INFO, "ARM9 BIOS not found. Loading FreeBIOS.\n");
    if (ReadFile(system + "/bios7.bin", file) && file.size() == ARM7BIOSSize)
        memcpy(args.ARM7BIOS->data(), file.data(), ARM7BIOSSize);
    else
        Log(RETRO_LOG_INFO, "ARM7 BIOS not found. Loading FreeBIOS.\n");
    if (ReadFile(system + "/firmware.bin", file) && !file.empty())
    {
        Firmware firmware(file.data(), file.size());
        if (firmware.Buffer())
        {
            Log(RETRO_LOG_INFO, "FW: USER0 CRC16 = %s\n", firmware.GetEffectiveUserData().ChecksumValid() ? "GOOD" : "BAD");
            args.Firmware = std::move(firmware);
        }
    }

    nds = std::make_unique<NDS>(std::move(args));
    nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
    RendererSettings settings {1, false, false, false};
    nds->GetRenderer().SetRenderSettings(settings);

    romData.assign((const u8*)game->data, (const u8*)game->data + game->size);
    std::vector<u8> save;
    pendingSave.clear();
    saveDirty = false;
    NDSCart::NDSCartArgs cartArgs {};
    if (ReadFile(savePath, save) && !save.empty())
    {
        cartArgs.SRAM = std::make_unique<u8[]>(save.size());
        memcpy(cartArgs.SRAM.get(), save.data(), save.size());
        cartArgs.SRAMLength = save.size();
    }
    auto rom = std::make_unique<u8[]>(romData.size());
    memcpy(rom.get(), romData.data(), romData.size());
    auto cart = NDSCart::ParseROM(std::move(rom), romData.size(), nullptr, std::move(cartArgs));
    if (!cart)
    {
        Log(RETRO_LOG_ERROR, "melonDS could not parse the ROM\n");
        nds.reset();
        return false;
    }
    nds->SetNDSCart(std::move(cart));
    BootCart();
    return true;
}

RETRO_API bool retro_load_game_special(unsigned, const retro_game_info*, size_t) { return false; }

RETRO_API void retro_unload_game(void)
{
    FlushSave(true);
    nds.reset();
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

// A state is written into one buffer kept for the session (melonDS's own Savestate would allocate
// 32 MB each time); it grows if a state ever outgrows it.
static std::vector<u8> stateBuffer;

static u32 SaveToBuffer()
{
    if (stateBuffer.empty())
        stateBuffer.resize(24 * 1024 * 1024);
    for (;;)
    {
        Savestate state(stateBuffer.data(), stateBuffer.size(), true);
        nds->DoSavestate(&state);
        state.Finish();
        if (!state.Error)
            return state.Length();
        if (stateBuffer.size() >= 256 * 1024 * 1024)
            return 0;
        stateBuffer.resize(stateBuffer.size() * 2);
    }
}

RETRO_API size_t retro_serialize_size(void)
{
    return nds ? SaveToBuffer() : 0;
}

RETRO_API bool retro_serialize(void* data, size_t size)
{
    if (!nds)
        return false;
    const u32 length = SaveToBuffer();
    if (!length || length > size)
        return false;
    memcpy(data, stateBuffer.data(), length);
    return true;
}

RETRO_API bool retro_unserialize(const void* data, size_t size)
{
    if (!nds)
        return false;
    // melonDS checks the length in the state's header; Recore may pass a zero-padded buffer
    if (size < 12)
        return false;
    u32 length;
    memcpy(&length, (const u8*)data + 8, 4);
    if (length > size)
        return false;
    Savestate state(const_cast<void*>(data), length, false);
    if (state.Error)
        return false;
    return nds->DoSavestate(&state) && !state.Error;
}

RETRO_API void* retro_get_memory_data(unsigned id)
{
    return nds && id == RETRO_MEMORY_SYSTEM_RAM ? nds->MainRAM : nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
    return nds && id == RETRO_MEMORY_SYSTEM_RAM ? 0x400000 : 0;
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}
