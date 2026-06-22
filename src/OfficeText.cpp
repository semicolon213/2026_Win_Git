#include "../include/OfficeText.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <string>

// miniz는 이 파일에서만 사용한다 (다른 코드와 매크로 충돌 방지).
// 구현부를 이 번역 단위에 포함시키기 위해 MINIZ_IMPLEMENTATION 정의.
#define MINIZ_IMPLEMENTATION
#include "../include/miniz_single.h"

namespace
{
    // pptx 내용 비교용 최대 크기 (100MB). 더 크면 생략.
    constexpr unsigned long long kMaxPptxBytes = 100ULL * 1024 * 1024;

    // UTF-8 바이트열을 wstring으로 변환 (실패 시 빈 문자열)
    std::wstring Utf8ToWide(const char* data, size_t len)
    {
        std::wstring result;
        if (len == 0)
        {
            return result;
        }
        int needed = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), nullptr, 0);
        if (needed > 0)
        {
            result.resize(needed);
            MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), &result[0], needed);
        }
        return result;
    }

    // XML에서 흔한 엔티티를 실제 문자로 복원 (&amp; 등)
    std::wstring DecodeXmlEntities(const std::wstring& text)
    {
        std::wstring out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); )
        {
            if (text[i] == L'&')
            {
                if (text.compare(i, 5, L"&amp;") == 0) { out += L'&'; i += 5; continue; }
                if (text.compare(i, 4, L"&lt;") == 0) { out += L'<'; i += 4; continue; }
                if (text.compare(i, 4, L"&gt;") == 0) { out += L'>'; i += 4; continue; }
                if (text.compare(i, 6, L"&quot;") == 0) { out += L'"'; i += 6; continue; }
                if (text.compare(i, 6, L"&apos;") == 0) { out += L'\''; i += 6; continue; }
            }
            out += text[i];
            ++i;
        }
        return out;
    }

    // 슬라이드 XML 한 개에서 <a:t>...</a:t> 사이의 텍스트들을 모아 lines에 추가.
    // slideNum이 양수면 각 줄 앞에 "[N장] "을 붙여 어느 슬라이드인지 표시한다.
    void ExtractTextFromSlideXml(const std::string& xml, int slideNum, std::vector<std::wstring>& lines)
    {
        const std::string openTag = "<a:t>";
        const std::string closeTag = "</a:t>";

        // 슬라이드 번호 접두사 만들기 (예: "[2장] ")
        std::wstring prefix;
        if (slideNum > 0)
        {
            wchar_t buf[24]{};
            _snwprintf_s(buf, _TRUNCATE, L"[%d장] ", slideNum);
            prefix = buf;
        }

        size_t pos = 0;
        while (true)
        {
            size_t start = xml.find(openTag, pos);
            if (start == std::string::npos)
            {
                break;
            }
            start += openTag.size();
            size_t end = xml.find(closeTag, start);
            if (end == std::string::npos)
            {
                break;
            }

            std::wstring text = Utf8ToWide(xml.data() + start, end - start);
            text = DecodeXmlEntities(text);
            if (!text.empty())
            {
                lines.push_back(prefix + text);
            }
            pos = end + closeTag.size();
        }
    }

    // "ppt/slides/slideN.xml" 형태의 슬라이드 파일인지 판별
    bool IsSlideXmlName(const char* name)
    {
        // 접두 "ppt/slides/slide" + 숫자 + ".xml"
        const char* prefix = "ppt/slides/slide";
        size_t plen = strlen(prefix);
        if (strncmp(name, prefix, plen) != 0)
        {
            return false;
        }
        const char* p = name + plen;
        if (!(*p >= '0' && *p <= '9'))
        {
            return false;  // slide 뒤에 숫자가 와야 함 (slideLayout 등 제외)
        }
        while (*p >= '0' && *p <= '9')
        {
            ++p;
        }
        return strcmp(p, ".xml") == 0;
    }

    // 슬라이드 번호 추출 (정렬용). 실패 시 큰 값.
    int SlideNumber(const char* name)
    {
        const char* prefix = "ppt/slides/slide";
        const char* p = name + strlen(prefix);
        int num = 0;
        bool any = false;
        while (*p >= '0' && *p <= '9')
        {
            num = num * 10 + (*p - '0');
            any = true;
            ++p;
        }
        return any ? num : 1000000;
    }

    // docx의 word/document.xml에서 <w:t ...>텍스트</w:t>를 모아 lines에 추가.
    // <w:t>는 속성이 붙을 수 있고(<w:t xml:space="preserve">), 빈 태그(<w:t/>)도 있으므로 함께 처리한다.
    void ExtractTextFromDocXml(const std::string& xml, std::vector<std::wstring>& lines)
    {
        size_t pos = 0;
        while (true)
        {
            size_t tagStart = xml.find("<w:t", pos);
            if (tagStart == std::string::npos)
            {
                break;
            }
            // "<w:t" 다음 글자가 '>' 또는 ' '(속성) 또는 '/'여야 진짜 w:t 태그 (<w:tbl> 등 배제)
            char after = xml[tagStart + 4];
            if (after != '>' && after != ' ' && after != '/')
            {
                pos = tagStart + 4;
                continue;
            }
            size_t gt = xml.find('>', tagStart);
            if (gt == std::string::npos)
            {
                break;
            }
            if (xml[gt - 1] == '/')  // <w:t/> 빈 태그
            {
                pos = gt + 1;
                continue;
            }
            size_t textStart = gt + 1;
            size_t close = xml.find("</w:t>", textStart);
            if (close == std::string::npos)
            {
                break;
            }

            std::wstring text = Utf8ToWide(xml.data() + textStart, close - textStart);
            text = DecodeXmlEntities(text);
            if (!text.empty())
            {
                lines.push_back(text);
            }
            pos = close + 6;
        }
    }
}

bool IsPptxFileName(const std::wstring& name)
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
    return ext == L".pptx";
}

std::vector<std::wstring> ExtractPptxText(const std::wstring& fullPath, unsigned long long sizeBytes, int* outSlideCount)
{
    std::vector<std::wstring> lines;
    if (outSlideCount)
    {
        *outSlideCount = 0;  // 실패 시 0 (호출 측에서 0이면 비교 안 함)
    }

    if (sizeBytes > kMaxPptxBytes)
    {
        return lines;  // 너무 큰 파일은 생략
    }

    // pptx(zip) 열기. miniz는 와이드 경로를 직접 안 받으므로, 핸들로 직접 읽어 메모리에 올린다.
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
        return lines;  // 열기 실패 시 조용히 생략
    }

    // 파일 전체를 메모리로 읽기
    std::string raw;
    char buffer[64 * 1024];
    DWORD bytesRead = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
    {
        raw.append(buffer, bytesRead);
    }
    CloseHandle(file);

    if (raw.empty())
    {
        return lines;
    }

    // miniz로 메모리상의 zip 열기
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, raw.data(), raw.size(), 0))
    {
        return lines;  // zip 파싱 실패 (손상/형식 불일치) -> 생략
    }

    // 슬라이드 XML들을 번호 순으로 모으기
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    std::vector<std::pair<int, mz_uint>> slides;  // (슬라이드 번호, zip 인덱스)
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&zip, i, &st) && IsSlideXmlName(st.m_filename))
        {
            slides.push_back({ SlideNumber(st.m_filename), i });
        }
    }
    std::sort(slides.begin(), slides.end());

    // 슬라이드 장 수 기록 (빈 장도 포함). 0이면 호출 측에서 비교하지 않는다.
    if (outSlideCount)
    {
        *outSlideCount = static_cast<int>(slides.size());
    }

    // 각 슬라이드에서 텍스트 추출 (슬라이드 번호 포함)
    for (const auto& s : slides)
    {
        size_t outSize = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, s.second, &outSize, 0);
        if (p)
        {
            std::string xml(static_cast<char*>(p), outSize);
            mz_free(p);
            ExtractTextFromSlideXml(xml, s.first, lines);  // s.first = 슬라이드 번호
        }
    }

    mz_zip_reader_end(&zip);
    return lines;
}

bool IsDocxFileName(const std::wstring& name)
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
    return ext == L".docx";
}

std::vector<std::wstring> ExtractDocxText(const std::wstring& fullPath, unsigned long long sizeBytes)
{
    std::vector<std::wstring> lines;

    if (sizeBytes > kMaxPptxBytes)
    {
        return lines;  // 너무 큰 파일은 생략 (pptx와 같은 한도 재사용)
    }

    // docx(zip) 열기. pptx와 동일하게 핸들로 읽어 메모리에 올린다.
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
        return lines;
    }

    std::string raw;
    char buffer[64 * 1024];
    DWORD bytesRead = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
    {
        raw.append(buffer, bytesRead);
    }
    CloseHandle(file);

    if (raw.empty())
    {
        return lines;
    }

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, raw.data(), raw.size(), 0))
    {
        return lines;
    }

    // docx 본문은 word/document.xml 한 곳에 있다 (pptx의 슬라이드 여러 개와 달리 단일 파일).
    int idx = mz_zip_reader_locate_file(&zip, "word/document.xml", nullptr, 0);
    if (idx >= 0)
    {
        size_t outSize = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(idx), &outSize, 0);
        if (p)
        {
            std::string xml(static_cast<char*>(p), outSize);
            mz_free(p);
            ExtractTextFromDocXml(xml, lines);
        }
    }

    mz_zip_reader_end(&zip);
    return lines;
}