#include "../include/FileExplorer.h"

#include "../include/dprint.h"
#include "../include/OfficeText.h"

#include <algorithm>

#include <cwctype>

#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE;
    constexpr DWORD kDebounceMs = 350;
    constexpr size_t kMaxEvents = 200;

    bool IsDotEntry(const wchar_t* name)
    {
        return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
    }

    // Office 등이 저장 중 만드는 임시/잠금 파일인지 판별 (Live Changes 노이즈 제거).
    // 예: pptE837.tmp, 44E4AD9D.tmp, ~$예제.pptx
    bool IsTempOrLockName(const std::wstring& name)
    {
        if (name.empty())
        {
            return false;
        }
        if (name[0] == L'~')  // ~$ 로 시작하는 Office 잠금 파일
        {
            return true;
        }
        // .tmp 확장자 (대소문자 무시)
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos)
        {
            std::wstring ext = name.substr(dot);
            for (wchar_t& ch : ext)
            {
                ch = static_cast<wchar_t>(towlower(ch));
            }
            if (ext == L".tmp")
            {
                return true;
            }
        }
        return false;
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

    // 두 문자열의 글자 단위 LCS 길이 표를 만들어 바뀐 부분만 +추가/-삭제로 표시
    // 예: "안녕하세요" -> 안녕히가세요" 면 "-하 +히가
    std::wstring DiffLineChars(const std::wstring& before, const std::wstring& after)
    {
        // 양쪽이 같으면 변화 없음
        if (before == after)
        {
            return L"";
        }

        const size_t kMaxLen = 2000;  // 한 줄이 이보다 길면 글자 LCS 생략(통째로 표시)
        if (before.size() > kMaxLen || after.size() > kMaxLen)
        {
            std::wstring result;
            if (!before.empty())
            {
                result += L"-" + before;
            }
            if (!after.empty())
            {
                if (!result.empty()) result += L" ";
                result += L"+" + after;
            }
            return result;
        }

        size_t n = before.size();
        size_t m = after.size();

        // LCS 길이 표 (n+1) x (m+1)
        std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));
        for (size_t i = 1; i <= n; ++i)
        {
            for (size_t j = 1; j <= m; ++j)
            {
                if (before[i - 1] == after[j - 1])
                {
                    lcs[i][j] = lcs[i - 1][j - 1] + 1;
                }
                else
                {
                    lcs[i][j] = (lcs[i - 1][j] >= lcs[i][j - 1]) ? lcs[i - 1][j] : lcs[i][j - 1];
                }
            }
        }

        // 역추적하며 삭제(-)/추가(+) 글자를 수집
        std::wstring removed;
        std::wstring added;
        size_t i = n, j = m;
        std::wstring result;

        // 뒤에서부터 모으므로 임시 버퍼에 담고 마지막에 뒤집기
        std::wstring delBuf, addBuf;
        while (i > 0 && j > 0)
        {
            if (before[i - 1] == after[j - 1])
            {
                --i; --j;  // 공통 글자: 표시 안 함
            }
            else if (lcs[i - 1][j] >= lcs[i][j - 1])
            {
                delBuf += before[i - 1];  // 삭제된 글자
                --i;
            }
            else
            {
                addBuf += after[j - 1];   // 추가된 글자
                --j;
            }
        }
        while (i > 0) { delBuf += before[i - 1]; --i; }
        while (j > 0) { addBuf += after[j - 1]; --j; }

        std::reverse(delBuf.begin(), delBuf.end());
        std::reverse(addBuf.begin(), addBuf.end());

        if (!delBuf.empty())
        {
            result += L"-" + delBuf;
        }
        if (!addBuf.empty())
        {
            if (!result.empty()) result += L" ";
            result += L"+" + addBuf;
        }
        return result;
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

// 텍스트 파일을 줄 단위로 읽어 vector로 반환 - 대상이 아니거나 읽기 실패 시 빈 vector
// 파일을 직접 읽으므로 반드시 락 밖에서 호출해야
std::vector<std::wstring> FileExplorer::ReadTextLines(const std::wstring& fullPath, ULONGLONG sizeBytes) const
{
    std::vector<std::wstring> lines;
    if (sizeBytes > kMaxLineCountBytes)
    {
        return lines;  // 너무 큰 파일은 내용 비교 생략
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
        return lines;  // 열기 실패 시 빈 결과 (조용히 생략)
    }

    // 파일 전체를 바이트로 읽어들임
    std::string raw;
    char buffer[8 * 1024];
    DWORD bytesRead = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
    {
        raw.append(buffer, bytesRead);
    }
    CloseHandle(file);

    // UTF-8 -> UTF-16(wstring) 변환 실패하면 빈 결과
    std::wstring text;
    if (!raw.empty())
    {
        int needed = MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
        if (needed > 0)
        {
            text.resize(needed);
            MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), &text[0], needed);
        }
    }

    // 줄 단위로 분할 (\r\n, \n 모두 처리)
    std::wstring current;
    for (wchar_t ch : text)
    {
        if (ch == L'\n')
        {
            lines.push_back(current);
            current.clear();
        }
        else if (ch != L'\r')
        {
            current += ch;
        }
    }
    lines.push_back(current);  // 마지막 줄 (개행으로 안 끝나도 포함)

    return lines;
}

// 2단계 LCS diff: 먼저 줄 단위로 비교해 바뀐 줄을 찾고
// 짝지어지는 줄은 글자 단위(DiffLineChars)로, 순수 추가/삭제 줄은 통째로 표시
std::wstring FileExplorer::BuildContentDiff(const std::vector<std::wstring>& before, const std::vector<std::wstring>& after) const
{
    size_t n = before.size();
    size_t m = after.size();

    // 안전벨트 줄 수가 매우 많으면 메모리를 폭증 위험이므로 수 변화만 표시
    const size_t kMaxLcsLines = 3000;
    if (n > kMaxLcsLines || m > kMaxLcsLines)
    {
        if (m > n)
        {
            wchar_t buf[48]{};
            _snwprintf_s(buf, _TRUNCATE, L"+%zu줄(상세생략)", m - n);
            return buf;
        }
        else if (n > m)
        {
            wchar_t buf[48]{};
            _snwprintf_s(buf, _TRUNCATE, L"-%zu줄(상세생략)", n - m);
            return buf;
        }
        return L"내용 변경(상세생략)";
    }

    // 줄 단위 LCS 표
    std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = 1; i <= n; ++i)
    {
        for (size_t j = 1; j <= m; ++j)
        {
            if (before[i - 1] == after[j - 1])
            {
                lcs[i][j] = lcs[i - 1][j - 1] + 1;
            }
            else
            {
                lcs[i][j] = (lcs[i - 1][j] >= lcs[i][j - 1]) ? lcs[i - 1][j] : lcs[i][j - 1];
            }
        }
    }

    // 역추적하며 변경된 줄들을 수집 (앞에서부터 순서대로 담기 위해 뒤에서 모아 뒤집음)
    size_t i = n, j = m;
    std::vector<std::pair<int, std::wstring>> rev;  // (종류, 텍스트) 종류: 0=삭제줄, 1=추가줄

    while (i > 0 && j > 0)
    {
        if (before[i - 1] == after[j - 1])
        {
            --i; --j;  // 같은 줄: 변화 없음
        }
        else if (lcs[i - 1][j] >= lcs[i][j - 1])
        {
            rev.push_back({ 0, before[i - 1] });  // 삭제된 줄
            --i;
        }
        else
        {
            rev.push_back({ 1, after[j - 1] });   // 추가된 줄
            --j;
        }
    }
    while (i > 0) { rev.push_back({ 0, before[i - 1] }); --i; }
    while (j > 0) { rev.push_back({ 1, after[j - 1] }); --j; }

    std::reverse(rev.begin(), rev.end());

    // 인접한 삭제줄 + 추가줄쌍은 글자 단위 diff로 합쳐 표시 나머지는 통째로
    std::wstring result;
    size_t k = 0;
    while (k < rev.size())
    {
        std::wstring piece;

        // 쌍 판정: (삭제 다음 추가) 또는 (추가 다음 삭제)
        if (k + 1 < rev.size() && rev[k].first != rev[k + 1].first)
        {
            const std::wstring& delLine = (rev[k].first == 0) ? rev[k].second : rev[k + 1].second;
            const std::wstring& addLine = (rev[k].first == 1) ? rev[k].second : rev[k + 1].second;
            piece = DiffLineChars(delLine, addLine);
            k += 2;
        }
        else if (rev[k].first == 0)
        {
            if (!rev[k].second.empty())  // 빈 줄 삭제는 표시 생략
            {
                piece = L"-" + rev[k].second;
            }
            ++k;
        }
        else
        {
            if (!rev[k].second.empty())  // 빈 줄 추가는 표시 생략
            {
                piece = L"+" + rev[k].second;
            }
            ++k;
        }

        if (piece.empty())
        {
            continue;  // 빈 변경(빈 줄끼리 등)은 건너뜀
        }
        if (!result.empty())
        {
            result += L" / ";
        }
        result += piece;
    }

    return result;
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

    // 내용 변경 diff 표시 (양쪽 다 줄 내용이 있는 텍스트 파일일 때만)
    if (!before.lines.empty() || !after.lines.empty())
    {
        std::wstring contentDiff = BuildContentDiff(before.lines, after.lines);
        if (!contentDiff.empty())
        {
            if (!result.empty())
            {
                result += L", ";
            }
            result += contentDiff;
        }
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
            entry.lines = ReadTextLines(fullPath, entry.sizeBytes);  // 줄 내용 (diff 비교용)
            if (!entry.lines.empty())
            {
                entry.lineCount = static_cast<int>(entry.lines.size());
            }
        }
        else if (!entry.isDirectory && IsPptxFileName(entry.name))
        {
            // pptx는 슬라이드 텍스트를 추출해 줄 내용으로 사용 (내용 diff 대상)
            std::wstring fullPath = path;
            if (!fullPath.empty() && fullPath.back() != L'\\')
            {
                fullPath += L'\\';
            }
            fullPath += entry.name;

            // 수정시각+크기가 직전 추출과 같으면 ZIP을 다시 열지 않고 캐시를 재사용
            // PowerPoint가 저장 중 파일을 여러 번 건드릴 때 불완전한 재추출을 줄여줌
            ULONGLONG curTime = (static_cast<ULONGLONG>(entry.lastWriteTime.dwHighDateTime) << 32)
                | entry.lastWriteTime.dwLowDateTime;
            auto stampIt = m_officeStamp.find(entry.name);
            auto cacheIt = m_pptxCache.find(entry.name);
            auto countIt = m_pptxSlideCount.find(entry.name);
            if (stampIt != m_officeStamp.end() &&
                stampIt->second.writeTime == curTime &&
                stampIt->second.sizeBytes == entry.sizeBytes &&
                cacheIt != m_pptxCache.end() &&
                countIt != m_pptxSlideCount.end())
            {
                // 안 바뀜: 캐시 재사용 (ZIP 추출 생략)
                entry.lines = cacheIt->second;
                entry.lineCount = countIt->second;
            }
            else
            {
                // 바뀜(또는 첫 스캔): 실제로 추출
                int slideCount = 0;
                entry.lines = ExtractPptxText(fullPath, entry.sizeBytes, &slideCount);
                entry.lineCount = slideCount;  // pptx는 lineCount를 "장 수"로 사용 (0이면 읽기 실패/저장 중)
                // 추출 성공(슬라이드 1개 이상)일 때만 stamp를 기록
                // 저장 중이라 0장으로 읽힌 경우엔 기록하지 않아 다음 스캔에서 다시 추출
                if (slideCount > 0)
                {
                    m_officeStamp[entry.name] = { curTime, entry.sizeBytes };
                }
            }
        }
        else if (!entry.isDirectory && IsDocxFileName(entry.name))
        {
            // docx는 본문 텍스트를 추출해 내용 diff 대상으로 사용 (장 개념 없음)
            std::wstring fullPath = path;
            if (!fullPath.empty() && fullPath.back() != L'\\')
            {
                fullPath += L'\\';
            }
            fullPath += entry.name;

            // 수정시각+크기가 직전 추출과 같으면 ZIP을 다시 열지 않고 캐시를 재사용한다.
            ULONGLONG curTime = (static_cast<ULONGLONG>(entry.lastWriteTime.dwHighDateTime) << 32)
                | entry.lastWriteTime.dwLowDateTime;
            auto stampIt = m_officeStamp.find(entry.name);
            auto cacheIt = m_docxCache.find(entry.name);
            if (stampIt != m_officeStamp.end() &&
                stampIt->second.writeTime == curTime &&
                stampIt->second.sizeBytes == entry.sizeBytes &&
                cacheIt != m_docxCache.end())
            {
                // 안 바뀜: 캐시 재사용 (ZIP 추출 생략)
                entry.lines = cacheIt->second;
                entry.lineCount = static_cast<int>(entry.lines.size());
            }
            else
            {
                // 바뀜(또는 첫 스캔): 실제로 추출
                entry.lines = ExtractDocxText(fullPath, entry.sizeBytes);
                if (!entry.lines.empty())
                {
                    entry.lineCount = static_cast<int>(entry.lines.size());
                    // 추출 성공일 때만 stamp 기록 (저장 중 빈 결과는 기록 안 함)
                    m_officeStamp[entry.name] = { curTime, entry.sizeBytes };
                }
            }
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

            // pptx는 용량 변화가 압축 때문에 의미가 없으므로 이 루프에서 제외하고
            // 아래 전용 블록이 내용 diff만으로 detail을 전담 docx도 동일
            if (IsPptxFileName(entry.name) || IsDocxFileName(entry.name))
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
                // Modified는 물론, PowerPoint 등은 저장 시 임시파일을 rename하므로
                // 같은 이름으로 새로 들어온 Rename+(RenamedNew)도 내용 비교 대상에 포함
                if (change.fileName == entry.name &&
                    (change.type == FileChangeType::Modified || change.type == FileChangeType::RenamedNew))
                {
                    change.detail = detail;
                    break;  // push_front로 넣으므로 가장 최근 항목이 앞쪽에
                }
            }
        }
    }

    // pptx 전용 비교: PowerPoint는 저장 시 파일을 잠깐 없앴다가 rename으로 되살리므로
    // m_previousFiles(스캔마다 clear)로는 이전 내용을 놓친다. 따로 유지되는 m_pptxCache로 비교
    for (const FileEntry& entry : scanned)
    {
        if (entry.isDirectory || !IsPptxFileName(entry.name))
        {
            continue;
        }

        // 부작용 방지: 장 수가 0이면 읽기 실패 또는 저장 중 불완전 상태이므로 비교 X
        int curSlides = entry.lineCount;  // pptx는 lineCount에 장 수가 들어있음
        if (curSlides <= 0)
        {
            continue;
        }

        // 장 수 변화 계산 (이전 장 수를 알고, 0보다 클 때만 신뢰)
        std::wstring slidePart;
        auto cachedCount = m_pptxSlideCount.find(entry.name);
        if (cachedCount != m_pptxSlideCount.end() && cachedCount->second > 0 &&
            cachedCount->second != curSlides)
        {
            int delta = curSlides - cachedCount->second;
            wchar_t buf[32]{};
            _snwprintf_s(buf, _TRUNCATE, L"슬라이드%+d", delta);  // 예: 슬라이드+1, 슬라이드-2
            slidePart = buf;
        }

        // 내용(텍스트) diff 계산
        std::wstring contentDiff;
        auto cached = m_pptxCache.find(entry.name);
        if (cached != m_pptxCache.end() && cached->second != entry.lines)
        {
            contentDiff = BuildContentDiff(cached->second, entry.lines);
        }

        // 장 수 변화나 내용 변화 중 하나라도 있으면 "수정:"으로 표시
        if (!slidePart.empty() || !contentDiff.empty())
        {
            std::wstring body;
            if (!slidePart.empty())
            {
                body = slidePart;
            }
            if (!contentDiff.empty())
            {
                if (!body.empty()) body += L" | ";  // 장 변화와 텍스트 변화를 시각적으로 분리
                body += L"텍스트: " + contentDiff;   // 어느 장의 텍스트인지는 contentDiff 안의 [N장]으로 표시됨
            }

            std::wstring afterTime = NowTimeString();
            std::wstring newDetail = L"수정: " + body + (afterTime.empty() ? L"" : (L", " + afterTime));

            ScopedCriticalSection lock(&m_state.cs);
            for (auto& change : m_state.changes)
            {
                if (change.fileName == entry.name &&
                    (change.type == FileChangeType::Modified || change.type == FileChangeType::RenamedNew))
                {
                    change.detail = newDetail;
                    break;
                }
            }
        }
        else
        {
            // 변화 없는 pptx 이벤트(저장 과정 중간 rename 등)는 용량 등 노이즈를 정리
            // 단, 직전에 "수정:"이 잡힌 이벤트는 덮지 않고 보존
            ScopedCriticalSection lock(&m_state.cs);
            for (auto& change : m_state.changes)
            {
                if (change.fileName == entry.name &&
                    (change.type == FileChangeType::Modified || change.type == FileChangeType::RenamedNew))
                {
                    if (change.detail.rfind(L"수정: ", 0) != 0)
                    {
                        change.detail = NowTimeString();
                    }
                    break;
                }
            }
        }

        // 캐시 갱신 (사라져도 유지되도록 여기서만 갱신)
        m_pptxCache[entry.name] = entry.lines;
        m_pptxSlideCount[entry.name] = curSlides;
    }

    // docx 전용 비교: Word도 저장 시 rename으로 교체하므로 따로 유지되는 m_docxCache로 비교
    // pptx와 달리 장 개념이 없어 순수 텍스트 diff만 표시
    for (const FileEntry& entry : scanned)
    {
        if (entry.isDirectory || !IsDocxFileName(entry.name) || entry.lines.empty())
        {
            continue;
        }

        auto cached = m_docxCache.find(entry.name);
        if (cached != m_docxCache.end() && cached->second != entry.lines)
        {
            std::wstring diff = BuildContentDiff(cached->second, entry.lines);
            if (!diff.empty())
            {
                std::wstring afterTime = NowTimeString();
                std::wstring newDetail = L"수정: 텍스트: " + diff + (afterTime.empty() ? L"" : (L", " + afterTime));

                ScopedCriticalSection lock(&m_state.cs);
                for (auto& change : m_state.changes)
                {
                    if (change.fileName == entry.name &&
                        (change.type == FileChangeType::Modified || change.type == FileChangeType::RenamedNew))
                    {
                        change.detail = newDetail;
                        break;
                    }
                }
            }
        }
        else
        {
            // 변화 없는 docx 이벤트(저장 과정 중간 rename 등)는 노이즈를 정리
            // 단, 직전에 "수정:"이 잡힌 이벤트는 보존한다.
            ScopedCriticalSection lock(&m_state.cs);
            for (auto& change : m_state.changes)
            {
                if (change.fileName == entry.name &&
                    (change.type == FileChangeType::Modified || change.type == FileChangeType::RenamedNew))
                {
                    if (change.detail.rfind(L"수정: ", 0) != 0)
                    {
                        change.detail = NowTimeString();
                    }
                    break;
                }
            }
        }

        m_docxCache[entry.name] = entry.lines;
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
    // 16KB 변경 알림 버퍼를 스택이 아닌 힙에 (C6262 회피)
    // FILE_NOTIFY_INFORMATION은 DWORD 정렬이 필요하므로 DWORD 벡터로 잡아 정렬을 보장
    std::vector<DWORD> bufferStorage(16 * 1024 / sizeof(DWORD), 0);
    BYTE* buffer = reinterpret_cast<BYTE*>(bufferStorage.data());
    const DWORD bufferSize = static_cast<DWORD>(bufferStorage.size() * sizeof(DWORD));

    while (!m_stopRequested.load())
    {
        DWORD bytesReturned = 0;
        BOOL ok = ReadDirectoryChangesW(
            m_directoryHandle,
            buffer,
            bufferSize,
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
    m_pptxCache.clear();      // pptx 캐시도 폴더가 바뀌면 무효
    m_pptxSlideCount.clear(); // pptx 장 수 캐시도 무효
    m_docxCache.clear();      // docx 캐시도 무효
    m_officeStamp.clear();    // 오피스 파일 시각+크기 기록도 무효

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
    // Office 등의 임시/잠금 파일은 노이즈이므로 기록하지 않는다.
    if (IsTempOrLockName(fileName))
    {
        return;
    }

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