#include "../include/FileExplorer.h"

#include "../include/dprint.h"

#include <algorithm>

#include <cwctype>

namespace
{
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE;
constexpr DWORD kDebounceMs = 350;
constexpr size_t kMaxEvents = 200;

bool IsDotEntry(const wchar_t* name)
{
    return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
}

// 줄 수 계산(변경 확인)용 최대 크기 (100MB) 파일 목록 표시와는 무관, 더 큰 텍스트 파일은 줄 수만 생략하고 목록에는 정상 표시
constexpr ULONGLONG kMaxLineCountBytes = 100ULL * 1024 * 1024;

// 파일명이 텍스트 계열 확장자인지 판별 (줄 수 계산 대상만 true)
bool IsTextFileName(const std::wstring& name)
{
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos)
    {
        return false;
    }

    std::wstring ext = name.substr(dot);
    for (wchar_t& ch : ext)
    {
        ch = static_cast<wchar_t>(towlower(ch));
    }

    static const wchar_t* kTextExts[] = {
        L".txt", L".md", L".log", L".csv", L".json", L".xml", L".html", L".htm",
        L".css", L".js", L".ts", L".cpp", L".h", L".hpp", L".c", L".cc",
        L".py", L".java", L".cs", L".rs", L".go", L".rb", L".php", L".sql",
        L".sh", L".bat", L".ini", L".yml", L".yaml", L".cfg", L".conf"
    };

    for (const wchar_t* candidate : kTextExts)
    {
        if (ext == candidate)
        {
            return true;
        }
    }
    return false;
}

std::wstring TrimLeft(std::wstring value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](wchar_t ch) {
        return ch != L' ' && ch != L'\t';
    }));
    return value;
}
// 바이트 수를 읽을 수 있게 1536 -> 1.5KB
std::wstring ToReadableSize(ULONGLONG bytes)
{
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB" };
    double value = static_cast<double>(bytes);
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 3)
    {
        value /= 1024.0;
        ++unitIndex;
    }

    wchar_t buffer[64]{};
    if (unitIndex == 0)
    {
        _snwprintf_s(buffer, _TRUNCATE, L"%lluB", bytes);
    }
    else
    {
        _snwprintf_s(buffer, _TRUNCATE, L"%.1f%s", value, units[unitIndex]);
    }
    return buffer;
}
// FILETIME을 로컬 시각 "HH:MM:SS" 문자열로 변환 실패시 빈 문자열
std::wstring ToTimeString(const FILETIME& ft)
{
    FILETIME local{};
    if (!FileTimeToLocalFileTime(&ft, &local))
    {
        return L"";
    }

    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&local, &st))
    {
        return L"";
    }

    wchar_t buffer[16]{};
    _snwprintf_s(buffer, _TRUNCATE, L"%02d:%02d", st.wHour, st.wMinute);
    return buffer;
}
// 현재 로컬 시각을 "@HH:MM:SS" 형태로 반환 (이벤트 감지 시각용)
std::wstring NowTimeString()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t buffer[16]{};
    _snwprintf_s(buffer, _TRUNCATE, L"@%02d:%02d", st.wHour, st.wMinute);
    return buffer;
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

// 텍스트 파일의 줄 수를 세는 함수 대상이 아니거나(너무 큼) 읽기 실패 시 -1 반환.
// 파일을 직접 읽으므로 반드시 락 밖에서 호출해야
int FileExplorer::CountTextLines(const std::wstring& fullPath, ULONGLONG sizeBytes) const
{
    if (sizeBytes > kMaxLineCountBytes)
    {
        return -1;  // 너무 큰 파일은 줄 수 계산만 건너뜀 (목록 표시 영향 X)
    }

    HANDLE file = CreateFileW(
        fullPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE)
    {
        return -1;  // 열기 실패 시 생략 (잠김/삭제됨 등)
    }

    int lineCount = 0;
    bool sawAnyByte = false;
    char buffer[8 * 1024];
    DWORD bytesRead = 0;

    while (ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
    {
        sawAnyByte = true;
        for (DWORD i = 0; i < bytesRead; ++i)
        {
            if (buffer[i] == '\n')
            {
                ++lineCount;
            }
        }
    }

    CloseHandle(file);

    // 마지막 줄이 개행으로 끝나지 않아도 한 줄로 카운트
    if (sawAnyByte)
    {
        ++lineCount;
    }
    return lineCount;
}

std::wstring FileExplorer::BuildModifiedDetail(const FileEntry& before, const FileEntry& after) const
{
    bool sizeChanged = before.sizeBytes != after.sizeBytes;
    bool timeChanged = CompareFileTime(&before.lastWriteTime, &after.lastWriteTime) != 0;

    if (!sizeChanged && !timeChanged)
    {
        return L"";
    }

    std::wstring result;

    // 크기가 바뀌었으면 이전 -> 이후 표시
    if (sizeChanged)
    {
        result = ToReadableSize(before.sizeBytes) + L"→" + ToReadableSize(after.sizeBytes);
    }

    // 줄 수 변화 표시 (양쪽 다 계산된 텍스트 파일이고 줄 수가 다를 때만)
    if (before.lineCount >= 0 && after.lineCount >= 0 && before.lineCount != after.lineCount)
    {
        int diff = after.lineCount - before.lineCount;
        wchar_t lineBuf[32]{};
        _snwprintf_s(lineBuf, _TRUNCATE, L"%+d줄", diff);
        if (!result.empty())
        {
            result += L", ";
        }
        result += lineBuf;
    }

    // 파일 수정 시각 표시
    std::wstring afterTime = ToTimeString(after.lastWriteTime);
    if (!afterTime.empty())
    {
        if (!result.empty())
        {
            result += L", ";
        }
        result += L"@" + afterTime;
    }
    return result;
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
        // 텍스트 파일이면 줄 수 계산 (락 밖이라 UI를 막지 않음) 그 외는 -1 유지
        if (!entry.isDirectory && IsTextFileName(entry.name))
        {
            std::wstring fullPath = path;
            if (!fullPath.empty() && fullPath.back() != L'\\')
            {
                fullPath += L'\\';
            }
            fullPath += entry.name;
            entry.lineCount = CountTextLines(fullPath, entry.sizeBytes);
        }
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

   // 이전 스냅샷과 비교해 수정된 파일이 어떻게 바뀌었는지 설명
   // m_previousFiles는 이 감시 스레드에서만 접근하므로 이 스레드에서 락 필요 X
    if (!m_previousFiles.empty())
    {
        for (const FileEntry& entry : scanned)
        {
            if (entry.isDirectory)
            {
                continue;
            }

            auto found = m_previousFiles.find(entry.name);
            if (found == m_previousFiles.end())
            {
                continue;  // 새로 생긴 파일 -> 비교 대상이 없으므로 스킵
            }

            std::wstring detail = BuildModifiedDetail(found->second, entry);
            if (detail.empty())
            {
                continue;
            }

            ScopedCriticalSection lock(&m_state.cs);
            for (auto& change : m_state.changes)
            {
                if (change.fileName == entry.name && change.type == FileChangeType::Modified)
                {
                    change.detail = detail;
                    break;  // push_front로 넣으므로 가장 최근 항목이 앞쪽에
                }
            }
        }
    }

    m_previousFiles.clear();
    for (const FileEntry& entry : scanned)
    {
        if (!entry.isDirectory)
        {
            m_previousFiles[entry.name] = entry;
        }
    }
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
    m_previousFiles.clear();  // 이전 폴더의 스냅샷은 더 이상 유효하지 않음

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
    event.detail = NowTimeString();  // 감지 시각 기록
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
