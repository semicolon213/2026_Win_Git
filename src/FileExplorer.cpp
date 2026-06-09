#include "../include/FileExplorer.h"

#include "../include/dprint.h"

#include <algorithm>

namespace
{
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE;
constexpr DWORD kDebounceMs = 350;
constexpr size_t kMaxEvents = 200;

bool IsDotEntry(const wchar_t* name)
{
    return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
}

std::wstring TrimLeft(std::wstring value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](wchar_t ch) {
        return ch != L' ' && ch != L'\t';
    }));
    return value;
}
}

FileExplorer::FileExplorer(SharedState& state)
    : m_state(state),
      m_directoryHandle(INVALID_HANDLE_VALUE),
      m_threadHandle(nullptr),
      m_threadId(0),
      m_stopRequested(false)
{
    InitializeCriticalSection(&m_controlCs);
}

FileExplorer::~FileExplorer()
{
    Stop();
    DeleteCriticalSection(&m_controlCs);
}

bool FileExplorer::Start(const std::wstring& initialPath)
{
    ScopedCriticalSection lock(&m_controlCs);
    m_stopRequested.store(false);
    return RestartWatcherLocked(initialPath);
}

void FileExplorer::Stop()
{
    HANDLE threadToWait = nullptr;
    {
        ScopedCriticalSection lock(&m_controlCs);
        m_stopRequested.store(true);
        if (m_directoryHandle != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(m_directoryHandle, nullptr);
        }
        threadToWait = m_threadHandle;
    }

    if (threadToWait)
    {
        WaitForSingleObject(threadToWait, INFINITE);
        CloseHandle(threadToWait);
    }

    ScopedCriticalSection lock(&m_controlCs);
    m_threadHandle = nullptr;
    m_threadId = 0;
    CloseDirectoryHandle();
}

bool FileExplorer::ChangeDirectory(const std::wstring& command)
{
    std::wstring target = ResolveCdTarget(command);
    if (target.empty())
    {
        return false;
    }

    ScopedCriticalSection lock(&m_controlCs);
    return RestartWatcherLocked(target);
}

bool FileExplorer::ScanCurrentDirectory()
{
    std::wstring path;
    {
        ScopedCriticalSection lock(&m_controlCs);
        path = m_watchedPath;
    }

    if (path.empty())
    {
        return false;
    }

    return ScanDirectoryIntoState(path);
}

bool FileExplorer::ScanDirectoryIntoState(const std::wstring& path)
{
    std::vector<FileEntry> scanned;
    std::wstring pattern = path;
    if (!pattern.empty() && pattern.back() != L'\\')
    {
        pattern += L'\\';
    }
    pattern += L"*";

    WIN32_FIND_DATAW data{};
    HANDLE findHandle = FindFirstFileW(pattern.c_str(), &data);
    if (findHandle == INVALID_HANDLE_VALUE)
    {
        DPRINT_ERROR(L"FindFirstFileW failed: %s", path.c_str());
        return false;
    }

    do
    {
        if (IsDotEntry(data.cFileName))
        {
            continue;
        }

        FileEntry entry;
        entry.name = data.cFileName;
        entry.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.sizeBytes = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        entry.lastWriteTime = data.ftLastWriteTime;
        scanned.push_back(entry);
    } while (FindNextFileW(findHandle, &data));

    FindClose(findHandle);

    std::sort(scanned.begin(), scanned.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.isDirectory != b.isDirectory)
        {
            return a.isDirectory > b.isDirectory;
        }
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    {
        ScopedCriticalSection lock(&m_state.cs);
        m_state.files = std::move(scanned);
        m_state.currentPath = path;
    }

    return true;
}

DWORD WINAPI FileExplorer::WatchThreadThunk(LPVOID param)
{
    return static_cast<FileExplorer*>(param)->WatchLoop();
}

DWORD FileExplorer::WatchLoop()
{
    alignas(DWORD) BYTE buffer[16 * 1024]{};

    while (!m_stopRequested.load())
    {
        DWORD bytesReturned = 0;
        BOOL ok = ReadDirectoryChangesW(
            m_directoryHandle,
            buffer,
            static_cast<DWORD>(sizeof(buffer)),
            FALSE,
            kNotifyFilter,
            &bytesReturned,
            nullptr,
            nullptr);

        if (!ok)
        {
            DWORD error = GetLastError();
            if (m_stopRequested.load() || error == ERROR_OPERATION_ABORTED)
            {
                break;
            }
            DPRINT_WARN(L"ReadDirectoryChangesW failed: %lu", error);
            Sleep(100);
            continue;
        }

        BYTE* cursor = buffer;
        while (bytesReturned > 0)
        {
            auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(cursor);
            std::wstring fileName(info->FileName, info->FileNameLength / sizeof(wchar_t));
            PushChangeEvent(fileName, ConvertAction(info->Action));

            if (info->NextEntryOffset == 0)
            {
                break;
            }
            cursor += info->NextEntryOffset;
        }

        ScanCurrentDirectory();
    }

    return 0;
}

bool FileExplorer::RestartWatcherLocked(const std::wstring& newPath)
{
    m_stopRequested.store(true);
    if (m_directoryHandle != INVALID_HANDLE_VALUE)
    {
        CancelIoEx(m_directoryHandle, nullptr);
    }

    HANDLE oldThread = m_threadHandle;
    m_threadHandle = nullptr;
    m_threadId = 0;

    LeaveCriticalSection(&m_controlCs);
    if (oldThread)
    {
        WaitForSingleObject(oldThread, INFINITE);
        CloseHandle(oldThread);
    }
    EnterCriticalSection(&m_controlCs);

    CloseDirectoryHandle();
    m_stopRequested.store(false);
    m_watchedPath = newPath;

    if (!ScanDirectoryIntoState(newPath))
    {
        return false;
    }

    if (!OpenDirectoryHandle(newPath))
    {
        return false;
    }

    m_threadHandle = CreateThread(nullptr, 0, &FileExplorer::WatchThreadThunk, this, 0, &m_threadId);
    if (!m_threadHandle)
    {
        DPRINT_ERROR(L"CreateThread for watcher failed");
        CloseDirectoryHandle();
        return false;
    }

    DPRINT_INFO(L"Watching directory: %s", newPath.c_str());
    return true;
}

bool FileExplorer::OpenDirectoryHandle(const std::wstring& path)
{
    m_directoryHandle = CreateFileW(
        path.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);

    if (m_directoryHandle == INVALID_HANDLE_VALUE)
    {
        DPRINT_ERROR(L"CreateFileW directory handle failed: %s", path.c_str());
        return false;
    }
    return true;
}

void FileExplorer::CloseDirectoryHandle()
{
    if (m_directoryHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_directoryHandle);
        m_directoryHandle = INVALID_HANDLE_VALUE;
    }
}

void FileExplorer::PushChangeEvent(const std::wstring& fileName, FileChangeType type)
{
    ULONGLONG now = GetTickCount64();

    ScopedCriticalSection lock(&m_state.cs);

    // Coalesce events for the same file within the debounce window into one entry.
    for (auto& existing : m_state.changes)
    {
        if (existing.fileName == fileName && now - existing.tick < kDebounceMs)
        {
            existing.type = type;
            existing.tick = now;
            return;
        }
    }

    FileChangeEvent event;
    event.fileName = fileName;
    event.type = type;
    event.tick = now;
    m_state.changes.push_front(event);
    while (m_state.changes.size() > kMaxEvents)
    {
        m_state.changes.pop_back();
    }
}

FileChangeType FileExplorer::ConvertAction(DWORD action) const
{
    switch (action)
    {
    case FILE_ACTION_ADDED:
        return FileChangeType::Added;
    case FILE_ACTION_REMOVED:
        return FileChangeType::Removed;
    case FILE_ACTION_MODIFIED:
        return FileChangeType::Modified;
    case FILE_ACTION_RENAMED_OLD_NAME:
        return FileChangeType::RenamedOld;
    case FILE_ACTION_RENAMED_NEW_NAME:
        return FileChangeType::RenamedNew;
    default:
        return FileChangeType::Unknown;
    }
}

std::wstring FileExplorer::ResolveCdTarget(const std::wstring& command) const
{
    std::wstring trimmed = TrimLeft(command);
    if (trimmed.rfind(L"cd", 0) != 0)
    {
        return L"";
    }

    std::wstring arg = TrimLeft(trimmed.substr(2));
    if (arg.empty())
    {
        return L"";
    }

    wchar_t fullPath[MAX_PATH]{};
    std::wstring base = m_watchedPath;
    std::wstring target = arg;
    if (arg.size() >= 2 && arg[1] == L':')
    {
        target = arg;
    }
    else
    {
        if (!base.empty() && base.back() != L'\\')
        {
            base += L'\\';
        }
        target = base + arg;
    }

    DWORD length = GetFullPathNameW(target.c_str(), MAX_PATH, fullPath, nullptr);
    if (length == 0 || length >= MAX_PATH)
    {
        return L"";
    }

    DWORD attrs = GetFileAttributesW(fullPath);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return L"";
    }

    return fullPath;
}
