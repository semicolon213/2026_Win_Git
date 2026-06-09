#pragma once

#include "Common.h"

#include <fstream>
#include <string>

class CDPrint
{
public:
    static CDPrint& Instance();

    void Initialize(const std::wstring& logPath = L"dprint.log");
    void Shutdown();
    void Write(LogLevel level, const char* file, int line, const wchar_t* format, ...);

private:
    CDPrint();
    ~CDPrint();

    std::wstring LevelToString(LogLevel level) const;
    std::wstring FormatMessage(const wchar_t* format, va_list args) const;

    CRITICAL_SECTION m_cs;
    std::wofstream m_stream;
    bool m_initialized;
};

#ifdef MODE_TEST
#define DPRINT_DEBUG(...) CDPrint::Instance().Write(LogLevel::DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#else
#define DPRINT_DEBUG(...) ((void)0)
#endif

#define DPRINT_INFO(...)  CDPrint::Instance().Write(LogLevel::INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define DPRINT_WARN(...)  CDPrint::Instance().Write(LogLevel::WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define DPRINT_ERROR(...) CDPrint::Instance().Write(LogLevel::ERROR, __FILE__, __LINE__, __VA_ARGS__)
