#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Uncomment for full DEBUG/INFO/WARN/ERROR file logging.
// #define MODE_TEST

enum class LogLevel
{
    DEBUG = 0,
    INFO,
    WARN,
    ERROR
};

enum class FileChangeType
{
    Added,
    Removed,
    Modified,
    RenamedOld,
    RenamedNew,
    Unknown
};

struct ScopedCriticalSection
{
    CRITICAL_SECTION* cs;

    explicit ScopedCriticalSection(CRITICAL_SECTION* target) : cs(target)
    {
        EnterCriticalSection(cs);
    }

    ~ScopedCriticalSection()
    {
        LeaveCriticalSection(cs);
    }

    ScopedCriticalSection(const ScopedCriticalSection&) = delete;
    ScopedCriticalSection& operator=(const ScopedCriticalSection&) = delete;
};

struct FileEntry
{
    std::wstring name;
    bool isDirectory = false;
    ULONGLONG sizeBytes = 0;
    FILETIME lastWriteTime{};
    int lineCount = -1;  // 텍스트 파일의 줄 수 -1이면 미계산 또는 대상 아님
};

struct FileChangeEvent
{
    std::wstring fileName;
    FileChangeType type = FileChangeType::Unknown;
    ULONGLONG tick = 0;
    std::wstring detail;  // 수정 상세 표시 ( A -> B) 정보가 없으면 빈 문자
};

struct HardwareSnapshot
{
    double cpuUsage = 0.0;
    DWORDLONG totalMemoryMb = 0;
    DWORDLONG usedMemoryMb = 0;
    DWORDLONG freeMemoryMb = 0;
    DWORD memoryLoad = 0;
    ULONGLONG tick = 0;
    std::deque<int> cpuHistory;
    std::deque<int> memoryHistory;
};

struct SharedState
{
    CRITICAL_SECTION cs;
    std::wstring currentPath;
    std::vector<FileEntry> files;
    std::deque<FileChangeEvent> changes;
    HardwareSnapshot hardware;
    std::atomic<bool> running;

    SharedState() : running(true)
    {
        InitializeCriticalSection(&cs);
    }

    ~SharedState()
    {
        DeleteCriticalSection(&cs);
    }

    SharedState(const SharedState&) = delete;
    SharedState& operator=(const SharedState&) = delete;
};

inline std::wstring ChangeTypeToString(FileChangeType type)
{
    switch (type)
    {
    case FileChangeType::Added:
        return L"Added";
    case FileChangeType::Removed:
        return L"Deleted";
    case FileChangeType::Modified:
        return L"Modified";
    case FileChangeType::RenamedOld:
        return L"Rename-";
    case FileChangeType::RenamedNew:
        return L"Rename+";
    default:
        return L"Unknown";
    }
}

inline WORD AttributeForChange(FileChangeType type)
{
    switch (type)
    {
    case FileChangeType::Added:
        return FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    case FileChangeType::Removed:
        return FOREGROUND_RED | FOREGROUND_INTENSITY;
    case FileChangeType::Modified:
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    case FileChangeType::RenamedOld:
    case FileChangeType::RenamedNew:
        return FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    default:
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }
}