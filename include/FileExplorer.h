#pragma once

#include "Common.h"

#include <string>
#include <thread>
#include <unordered_map>

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
    std::wstring BuildModifiedDetail(const FileEntry& before, const FileEntry& after) const; // 수정 상세 문자열 생성
    int CountTextLines(const std::wstring& fullPath, ULONGLONG sizeBytes) const; // 텍스트 파일 줄 수 (대상 아니면 -1)
    std::vector<std::wstring> ReadTextLines(const std::wstring& fullPath, ULONGLONG sizeBytes) const; // 텍스트 파일을 줄 단위로 읽음
    std::wstring BuildContentDiff(const std::vector<std::wstring>& before, const std::vector<std::wstring>& after) const; // 2단계 LCS diff 문자열

    SharedState& m_state;
    CRITICAL_SECTION m_controlCs;
    HANDLE m_directoryHandle;
    HANDLE m_threadHandle;
    DWORD m_threadId;
    std::atomic<bool> m_stopRequested;
    std::wstring m_watchedPath;
    std::unordered_map<std::wstring, FileEntry> m_previousFiles; // 이전 스캔 결과 (파일명 -> 정보) 비교용
};