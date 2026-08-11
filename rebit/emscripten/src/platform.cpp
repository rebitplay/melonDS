#include "runtime_internal.h"

#include "LocalMP.h"
#include "Platform.h"
#include "SPI_Firmware.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace melonDS::Platform
{

struct FileHandle
{
    std::FILE* file = nullptr;
};

struct Thread
{
    explicit Thread(std::function<void()> function) : value(std::move(function)) {}
    std::thread value;
};

struct Semaphore
{
    std::mutex mutex;
    std::condition_variable condition;
    unsigned count = 0;
};

struct Mutex
{
    std::mutex value;
};

struct AACDecoder {};
struct DynamicLibrary {};

namespace
{

const auto ClockEpoch = std::chrono::steady_clock::now();

class ScheduledMultiplayerCall
{
public:
    ScheduledMultiplayerCall(void* userdata, rebit::MultiplayerOperation operation) noexcept
        : Userdata(userdata), Scheduled(rebit::EnterMultiplayerTurn(userdata, operation))
    {
    }

    ~ScheduledMultiplayerCall() noexcept
    {
        rebit::LeaveMultiplayerTurn(Userdata, Scheduled);
    }

private:
    void* Userdata;
    bool Scheduled;
};

std::string FileModeString(FileMode mode, bool exists)
{
    char access = 'r';
    if (mode & FileMode::Append)
        access = 'a';
    else if (mode & FileMode::Write)
        access = ((mode & FileMode::Preserve) && exists) || (mode & FileMode::NoCreate) ? 'r' : 'w';

    std::string value(1, access);
    if ((mode & FileMode::ReadWrite) == FileMode::ReadWrite)
        value.push_back('+');
    if (!(mode & FileMode::Text))
        value.push_back('b');
    return value;
}

}

void SignalStop(StopReason, void* userdata)
{
    rebit::SignalStopped(userdata);
}

std::string GetLocalFilePath(const std::string& filename)
{
    return filename;
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    if ((mode & (FileMode::ReadWrite | FileMode::Append)) == FileMode::None)
        return nullptr;

    const bool exists = std::filesystem::exists(path);
    if ((mode & FileMode::NoCreate) && !exists)
        return nullptr;

    std::FILE* file = std::fopen(path.c_str(), FileModeString(mode, exists).c_str());
    if (!file)
        return nullptr;
    return new FileHandle {file};
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string& name)
{
    return std::filesystem::exists(name);
}

bool LocalFileExists(const std::string& name)
{
    return FileExists(GetLocalFilePath(name));
}

bool CheckFileWritable(const std::string& path)
{
    if (auto* file = OpenFile(path, FileMode::Append))
    {
        CloseFile(file);
        return true;
    }
    return false;
}

bool CheckLocalFileWritable(const std::string& path)
{
    return CheckFileWritable(GetLocalFilePath(path));
}

bool CloseFile(FileHandle* file)
{
    if (!file)
        return false;
    const bool closed = std::fclose(file->file) == 0;
    delete file;
    return closed;
}

bool IsEndOfFile(FileHandle* file)
{
    return !file || std::feof(file->file) != 0;
}

bool FileReadLine(char* str, int count, FileHandle* file)
{
    return file && std::fgets(str, count, file->file) != nullptr;
}

u64 FilePosition(FileHandle* file)
{
    return file ? static_cast<u64>(std::ftell(file->file)) : 0;
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    if (!file)
        return false;
    int whence = SEEK_SET;
    if (origin == FileSeekOrigin::Current)
        whence = SEEK_CUR;
    else if (origin == FileSeekOrigin::End)
        whence = SEEK_END;
    return std::fseek(file->file, static_cast<long>(offset), whence) == 0;
}

void FileRewind(FileHandle* file)
{
    if (file)
        std::rewind(file->file);
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    return file ? std::fread(data, static_cast<std::size_t>(size), static_cast<std::size_t>(count), file->file) : 0;
}

bool FileFlush(FileHandle* file)
{
    return file && std::fflush(file->file) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    return file ? std::fwrite(data, static_cast<std::size_t>(size), static_cast<std::size_t>(count), file->file) : 0;
}

u64 FileWriteFormatted(FileHandle* file, const char* format, ...)
{
    if (!file)
        return 0;
    std::va_list arguments;
    va_start(arguments, format);
    const int written = std::vfprintf(file->file, format, arguments);
    va_end(arguments);
    return written > 0 ? static_cast<u64>(written) : 0;
}

u64 FileLength(FileHandle* file)
{
    if (!file)
        return 0;
    const long position = std::ftell(file->file);
    if (position < 0 || std::fseek(file->file, 0, SEEK_END) != 0)
        return 0;
    const long length = std::ftell(file->file);
    std::fseek(file->file, position, SEEK_SET);
    return length > 0 ? static_cast<u64>(length) : 0;
}

void Log(LogLevel level, const char* format, ...)
{
    if (level == LogLevel::Debug)
        return;
    static std::mutex logMutex;
    std::lock_guard<std::mutex> guard(logMutex);
    std::fputs("[melonds_dual] ", stderr);
    std::va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
}

Thread* Thread_Create(std::function<void()> function)
{
    return new Thread(std::move(function));
}

void Thread_Free(Thread* thread)
{
    if (!thread)
        return;
    if (thread->value.joinable())
        thread->value.join();
    delete thread;
}

void Thread_Wait(Thread* thread)
{
    if (thread && thread->value.joinable())
        thread->value.join();
}

Semaphore* Semaphore_Create()
{
    return new Semaphore();
}

void Semaphore_Free(Semaphore* semaphore)
{
    delete semaphore;
}

void Semaphore_Reset(Semaphore* semaphore)
{
    if (!semaphore)
        return;
    std::lock_guard<std::mutex> guard(semaphore->mutex);
    semaphore->count = 0;
}

void Semaphore_Wait(Semaphore* semaphore)
{
    if (!semaphore)
        return;
    std::unique_lock<std::mutex> lock(semaphore->mutex);
    semaphore->condition.wait(lock, [&] { return semaphore->count > 0; });
    --semaphore->count;
}

bool Semaphore_TryWait(Semaphore* semaphore, int timeoutMs)
{
    if (!semaphore)
        return false;
    std::unique_lock<std::mutex> lock(semaphore->mutex);
    if (timeoutMs <= 0)
    {
        if (semaphore->count == 0)
            return false;
    }
    else if (!semaphore->condition.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return semaphore->count > 0; }))
    {
        return false;
    }
    --semaphore->count;
    return true;
}

void Semaphore_Post(Semaphore* semaphore, int count)
{
    if (!semaphore || count <= 0)
        return;
    {
        std::lock_guard<std::mutex> guard(semaphore->mutex);
        semaphore->count += static_cast<unsigned>(count);
    }
    semaphore->condition.notify_all();
}

Mutex* Mutex_Create()
{
    return new Mutex();
}

void Mutex_Free(Mutex* mutex)
{
    delete mutex;
}

void Mutex_Lock(Mutex* mutex)
{
    if (mutex)
        mutex->value.lock();
}

void Mutex_Unlock(Mutex* mutex)
{
    if (mutex)
        mutex->value.unlock();
}

bool Mutex_TryLock(Mutex* mutex)
{
    return mutex && mutex->value.try_lock();
}

void Sleep(u64 microseconds)
{
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
}

u64 GetMSCount()
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ClockEpoch).count());
}

u64 GetUSCount()
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - ClockEpoch).count());
}

void WriteNDSSave(const u8* data, u32 length, u32, u32, void* userdata)
{
    rebit::StoreSave(data, length, userdata);
}

void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

void MP_Begin(void* userdata)
{
    ScheduledMultiplayerCall scheduled(userdata, rebit::MultiplayerOperation::Begin);
    if (auto* multiplayer = rebit::LocalMultiplayer())
        multiplayer->Begin(rebit::InstanceId(userdata));
}

void MP_End(void* userdata)
{
    ScheduledMultiplayerCall scheduled(userdata, rebit::MultiplayerOperation::End);
    if (auto* multiplayer = rebit::LocalMultiplayer())
        multiplayer->End(rebit::InstanceId(userdata));
}

int MP_SendPacket(u8* data, int length, u64 timestamp, void* userdata)
{
    ScheduledMultiplayerCall scheduled(userdata, rebit::MultiplayerOperation::SendPacket);
    if (auto* context = rebit::Context(userdata))
        ++context->packetsSent;
    auto* multiplayer = rebit::LocalMultiplayer();
    return multiplayer ? multiplayer->SendPacket(rebit::InstanceId(userdata), data, length, timestamp) : 0;
}

int MP_RecvPacket(u8* data, u64* timestamp, void* userdata)
{
    ScheduledMultiplayerCall scheduled(userdata, rebit::MultiplayerOperation::RecvPacket);
    auto* multiplayer = rebit::LocalMultiplayer();
    const int received = multiplayer ? multiplayer->RecvPacket(rebit::InstanceId(userdata), data, timestamp) : 0;
    if (received > 0)
        if (auto* context = rebit::Context(userdata))
            ++context->packetsReceived;
    return received;
}

int MP_SendCmd(u8* data, int length, u64 timestamp, void* userdata)
{
    ScheduledMultiplayerCall scheduled(userdata, rebit::MultiplayerOperation::SendCommand);
    if (auto* context = rebit::Context(userdata))
    {
        ++context->packetsSent;
        ++context->commands;
    }
    auto* multiplayer = rebit::LocalMultiplayer();
    const int sent = multiplayer ? multiplayer->SendCmd(rebit::InstanceId(userdata), data, length, timestamp) : 0;
    if (sent > 0)
        rebit::NoteMultiplayerCommand(userdata);
    return sent;
}

int MP_SendReply(u8* data, int length, u64 timestamp, u16 aid, void* userdata)
{
    ScheduledMultiplayerCall scheduled(userdata, rebit::MultiplayerOperation::SendReply);
    if (auto* context = rebit::Context(userdata))
    {
        ++context->packetsSent;
        ++context->replies;
    }
    auto* multiplayer = rebit::LocalMultiplayer();
    const int sent = multiplayer ? multiplayer->SendReply(rebit::InstanceId(userdata), data, length, timestamp, aid) : 0;
    if (sent > 0)
        rebit::NoteMultiplayerReply();
    return sent;
}

int MP_SendAck(u8* data, int length, u64 timestamp, void* userdata)
{
    ScheduledMultiplayerCall scheduled(userdata, rebit::MultiplayerOperation::SendAck);
    if (auto* context = rebit::Context(userdata))
        ++context->packetsSent;
    auto* multiplayer = rebit::LocalMultiplayer();
    return multiplayer ? multiplayer->SendAck(rebit::InstanceId(userdata), data, length, timestamp) : 0;
}

int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata)
{
    ScheduledMultiplayerCall scheduled(userdata, rebit::MultiplayerOperation::RecvHostPacket);
    auto* multiplayer = rebit::LocalMultiplayer();
    const int received = multiplayer ? multiplayer->RecvHostPacket(rebit::InstanceId(userdata), data, timestamp) : 0;
    if (received > 0)
        if (auto* context = rebit::Context(userdata))
            ++context->packetsReceived;
    return received;
}

u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidMask, void* userdata)
{
    ScheduledMultiplayerCall scheduled(userdata, rebit::MultiplayerOperation::RecvReplies);
    auto* multiplayer = rebit::LocalMultiplayer();
    const u16 replies = multiplayer ? multiplayer->RecvReplies(rebit::InstanceId(userdata), data, timestamp, aidMask) : 0;
    if (replies != 0)
        if (auto* context = rebit::Context(userdata))
            context->packetsReceived += static_cast<unsigned>(__builtin_popcount(replies));
    return replies;
}

int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32* frame, int width, int height, bool, void*)
{
    if (frame && width > 0 && height > 0)
        std::memset(frame, 0, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * sizeof(u32));
}

void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16* data, int maximumLength, void*)
{
    if (data && maximumLength > 0)
        std::memset(data, 0, static_cast<std::size_t>(maximumLength) * sizeof(s16));
    return maximumLength;
}

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }

bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0.0f; }

DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }

}
