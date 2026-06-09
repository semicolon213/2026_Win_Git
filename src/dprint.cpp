#include "../include/dprint.h"

#include <chrono>
#include <cstdarg>
#include <iomanip>
#include <sstream>

CDPrint& CDPrint::Instance()
{
    static CDPrint instance;
    return instance;
}

CDPrint::CDPrint() : m_initialized(false)
{
    InitializeCriticalSection(&m_cs);
}

CDPrint::~CDPrint()
{
    Shutdown();
    DeleteCriticalSection(&m_cs);
}

void CDPrint::Initialize(const std::wstring& logPath)
{
    ScopedCriticalSection lock(&m_cs);
    if (m_initialized)
    {
        return;
    }

    m_stream.open(logPath, std::ios::out | std::ios::app);
    m_initialized = m_stream.good();
}

void CDPrint::Shutdown()
{
    ScopedCriticalSection lock(&m_cs);
    if (m_stream.is_open())
    {
        m_stream.flush();
        m_stream.close();
    }
    m_initialized = false;
}

void CDPrint::Write(LogLevel level, const char* file, int line, const wchar_t* format, ...)
{
#ifndef MODE_TEST
    if (level == LogLevel::DEBUG)
    {
        return;
    }
#endif

    if (!m_initialized)
    {
        Initialize();
    }

    va_list args;
    va_start(args, format);
    std::wstring message = FormatMessage(format, args);
    va_end(args);

    ScopedCriticalSection lock(&m_cs);
    if (!m_stream.good())
    {
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto nowTime = std::chrono::system_clock::to_time_t(now);
    tm localTime{};
    localtime_s(&localTime, &nowTime);

    m_stream << L"["
             << std::put_time(&localTime, L"%Y-%m-%d %H:%M:%S")
             << L"][" << LevelToString(level) << L"] "
             << L"(" << file << L":" << line << L") "
             << message << std::endl;
}

std::wstring CDPrint::LevelToString(LogLevel level) const
{
    switch (level)
    {
    case LogLevel::DEBUG:
        return L"DEBUG";
    case LogLevel::INFO:
        return L"INFO";
    case LogLevel::WARN:
        return L"WARN";
    case LogLevel::ERROR:
        return L"ERROR";
    default:
        return L"UNKNOWN";
    }
}

std::wstring CDPrint::FormatMessage(const wchar_t* format, va_list args) const
{
    wchar_t buffer[1024]{};
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    return buffer;
}
