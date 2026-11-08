// melonDS 1.x Platform layer for the Recore frontend (recore_frontend.cpp): single thread, files on
// Emscripten's file system, no network, no camera. The frontend owns saves and the microphone.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

#include "Platform.h"
#include "SPI_Firmware.h"
#include "frontend_internal.h"

namespace melonDS::Platform
{

struct FileHandle
{
    std::FILE* file;
};

// Single thread: melonDS only creates threads for the threaded software renderer, which is off.
struct Thread {};
struct Semaphore
{
    int count = 0;
};
struct Mutex {};
struct AACDecoder {};
struct DynamicLibrary {};

void SignalStop(StopReason, void*) {}

std::string GetLocalFilePath(const std::string& filename)
{
    return filename;
}

static bool Exists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    if ((mode & (FileMode::ReadWrite | FileMode::Append)) == FileMode::None)
        return nullptr;
    const bool exists = Exists(path);
    if ((mode & FileMode::NoCreate) && !exists)
        return nullptr;

    std::string access;
    if (mode & FileMode::Append)
        access = "a";
    else if (mode & FileMode::Write)
        access = ((mode & FileMode::Preserve) && exists) || (mode & FileMode::NoCreate) ? "r" : "w";
    else
        access = "r";
    if ((mode & FileMode::ReadWrite) == FileMode::ReadWrite)
        access += "+";
    if (!(mode & FileMode::Text))
        access += "b";

    std::FILE* file = std::fopen(path.c_str(), access.c_str());
    return file ? new FileHandle {file} : nullptr;
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(path, mode);
}

bool FileExists(const std::string& name) { return Exists(name); }
bool LocalFileExists(const std::string& name) { return Exists(name); }

bool CheckFileWritable(const std::string& path)
{
    FileHandle* file = OpenFile(path, FileMode::Append);
    if (!file)
        return false;
    CloseFile(file);
    return true;
}

bool CheckLocalFileWritable(const std::string& path) { return CheckFileWritable(path); }

bool CloseFile(FileHandle* file)
{
    if (!file)
        return false;
    const bool ok = std::fclose(file->file) == 0;
    delete file;
    return ok;
}

bool IsEndOfFile(FileHandle* file) { return !file || std::feof(file->file); }
bool FileReadLine(char* str, int count, FileHandle* file) { return file && std::fgets(str, count, file->file); }
u64 FilePosition(FileHandle* file) { return file ? (u64)std::ftell(file->file) : 0; }

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    if (!file)
        return false;
    const int whence = origin == FileSeekOrigin::Current ? SEEK_CUR : origin == FileSeekOrigin::End ? SEEK_END : SEEK_SET;
    return std::fseek(file->file, (long)offset, whence) == 0;
}

void FileRewind(FileHandle* file)
{
    if (file)
        std::rewind(file->file);
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    return file ? std::fread(data, size, count, file->file) : 0;
}

bool FileFlush(FileHandle* file) { return file && std::fflush(file->file) == 0; }

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    return file ? std::fwrite(data, size, count, file->file) : 0;
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    if (!file)
        return 0;
    va_list args;
    va_start(args, fmt);
    const int written = std::vfprintf(file->file, fmt, args);
    va_end(args);
    return written > 0 ? written : 0;
}

u64 FileLength(FileHandle* file)
{
    if (!file)
        return 0;
    const long pos = std::ftell(file->file);
    std::fseek(file->file, 0, SEEK_END);
    const long len = std::ftell(file->file);
    std::fseek(file->file, pos, SEEK_SET);
    return len > 0 ? len : 0;
}

void Log(LogLevel level, const char* fmt, ...)
{
    if (level == LogLevel::Debug)
        return;
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    rebit::FrontendLog(level == LogLevel::Error ? 3 : level == LogLevel::Warn ? 2 : 1, line);
}

Thread* Thread_Create(std::function<void()>)
{
    Log(LogLevel::Error, "melonDS asked for a thread; this build is single-threaded\n");
    std::abort();
}
void Thread_Free(Thread*) {}
void Thread_Wait(Thread*) {}

Semaphore* Semaphore_Create() { return new Semaphore(); }
void Semaphore_Free(Semaphore* sema) { delete sema; }
void Semaphore_Reset(Semaphore* sema) { if (sema) sema->count = 0; }
void Semaphore_Wait(Semaphore* sema) { if (sema && sema->count > 0) sema->count--; }
bool Semaphore_TryWait(Semaphore* sema, int)
{
    if (!sema || sema->count <= 0)
        return false;
    sema->count--;
    return true;
}
void Semaphore_Post(Semaphore* sema, int count) { if (sema) sema->count += count; }

Mutex* Mutex_Create() { return new Mutex(); }
void Mutex_Free(Mutex* mutex) { delete mutex; }
void Mutex_Lock(Mutex*) {}
void Mutex_Unlock(Mutex*) {}
bool Mutex_TryLock(Mutex*) { return true; }

void Sleep(u64) {}

static const auto Epoch = std::chrono::steady_clock::now();
u64 GetMSCount()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Epoch).count();
}
u64 GetUSCount()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Epoch).count();
}

void WriteNDSSave(const u8* savedata, u32 savelen, u32, u32, void*)
{
    rebit::StoreNDSSave(savedata, savelen);
}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int, u64, void*) { return 0; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int, u64, void*) { return 0; }
int MP_SendReply(u8*, int, u64, u16, void*) { return 0; }
int MP_SendAck(u8*, int, u64, void*) { return 0; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }

int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32*, int, int, bool, void*) {}

void Mic_Start(void*) { rebit::MicOpened(true); }
void Mic_Stop(void*) { rebit::MicOpened(false); }
int Mic_ReadInput(s16* data, int maxlength, void*) { return rebit::MicRead(data, maxlength); }

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }

bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0.f; }

DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }

}
