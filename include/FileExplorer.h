#pragma once

#include "Common.h"

#include <string>
#include <thread>

class FileExplorer
{
public:
    explicit FileExplorer(SharedState& state);
    ~FileExplorer();

    bool Start(const std::wstring& initialPath);
    void Stop();
    bool ChangeDirectory(const std::wstring& command);
    bool ScanCurrentDirectory();

private:
    static DWORD WINAPI WatchThreadThunk(LPVOID param);
    DWORD WatchLoop();

    bool RestartWatcherLocked(const std::wstring& newPath);
    bool OpenDirectoryHandle(const std::wstring& path);
    bool ScanDirectoryIntoState(const std::wstring& path);
    void CloseDirectoryHandle();
    void PushChangeEvent(const std::wstring& fileName, FileChangeType type);
    FileChangeType ConvertAction(DWORD action) const;
    std::wstring ResolveCdTarget(const std::wstring& command) const;

    SharedState& m_state;
    CRITICAL_SECTION m_controlCs;
    HANDLE m_directoryHandle;
    HANDLE m_threadHandle;
    DWORD m_threadId;
    std::atomic<bool> m_stopRequested;
    std::wstring m_watchedPath;
};
