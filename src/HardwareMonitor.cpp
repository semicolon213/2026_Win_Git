#include "../include/HardwareMonitor.h"

#include "../include/dprint.h"

#include <algorithm>

namespace
{
constexpr size_t kHistoryLimit = 48;
}

HardwareMonitor::HardwareMonitor(SharedState& state)
    : m_state(state),
      m_threadHandle(nullptr),
      m_stopEvent(nullptr),
      m_prevIdle{},
      m_prevKernel{},
      m_prevUser{},
      m_hasPreviousCpuSample(false)
{
}

HardwareMonitor::~HardwareMonitor()
{
    Stop();
}

bool HardwareMonitor::Start()
{
    if (m_threadHandle)
    {
        return true;
    }

    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent)
    {
        return false;
    }

    m_threadHandle = CreateThread(nullptr, 0, &HardwareMonitor::ThreadThunk, this, 0, nullptr);
    if (!m_threadHandle)
    {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
        return false;
    }

    return true;
}

void HardwareMonitor::Stop()
{
    if (m_stopEvent)
    {
        SetEvent(m_stopEvent);
    }

    if (m_threadHandle)
    {
        WaitForSingleObject(m_threadHandle, INFINITE);
        CloseHandle(m_threadHandle);
        m_threadHandle = nullptr;
    }

    if (m_stopEvent)
    {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

DWORD WINAPI HardwareMonitor::ThreadThunk(LPVOID param)
{
    return static_cast<HardwareMonitor*>(param)->PollLoop();
}

DWORD HardwareMonitor::PollLoop()
{
    while (WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0)
    {
        FILETIME idle{}, kernel{}, user{};
        double cpuUsage = 0.0;
        if (GetSystemTimes(&idle, &kernel, &user))
        {
            cpuUsage = CalculateCpuUsage(idle, kernel, user);
        }

        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        GlobalMemoryStatusEx(&memory);

        {
            ScopedCriticalSection lock(&m_state.cs);
            m_state.hardware.cpuUsage = cpuUsage;
            m_state.hardware.totalMemoryMb = memory.ullTotalPhys / (1024 * 1024);
            m_state.hardware.freeMemoryMb = memory.ullAvailPhys / (1024 * 1024);
            m_state.hardware.usedMemoryMb = m_state.hardware.totalMemoryMb - m_state.hardware.freeMemoryMb;
            m_state.hardware.memoryLoad = memory.dwMemoryLoad;
            m_state.hardware.tick = GetTickCount64();

            m_state.hardware.cpuHistory.push_back(static_cast<int>(std::clamp(cpuUsage, 0.0, 100.0)));
            m_state.hardware.memoryHistory.push_back(static_cast<int>(memory.dwMemoryLoad));
            while (m_state.hardware.cpuHistory.size() > kHistoryLimit)
            {
                m_state.hardware.cpuHistory.pop_front();
            }
            while (m_state.hardware.memoryHistory.size() > kHistoryLimit)
            {
                m_state.hardware.memoryHistory.pop_front();
            }
        }

        WaitForSingleObject(m_stopEvent, 1000);
    }

    return 0;
}

double HardwareMonitor::CalculateCpuUsage(const FILETIME& idle, const FILETIME& kernel, const FILETIME& user)
{
    if (!m_hasPreviousCpuSample)
    {
        m_prevIdle = idle;
        m_prevKernel = kernel;
        m_prevUser = user;
        m_hasPreviousCpuSample = true;
        return 0.0;
    }

    ULONGLONG idleNow = FileTimeToUInt64(idle);
    ULONGLONG kernelNow = FileTimeToUInt64(kernel);
    ULONGLONG userNow = FileTimeToUInt64(user);
    ULONGLONG idlePrev = FileTimeToUInt64(m_prevIdle);
    ULONGLONG kernelPrev = FileTimeToUInt64(m_prevKernel);
    ULONGLONG userPrev = FileTimeToUInt64(m_prevUser);

    ULONGLONG systemDelta = (kernelNow - kernelPrev) + (userNow - userPrev);
    ULONGLONG idleDelta = idleNow - idlePrev;

    m_prevIdle = idle;
    m_prevKernel = kernel;
    m_prevUser = user;

    if (systemDelta == 0)
    {
        return 0.0;
    }

    return (static_cast<double>(systemDelta - idleDelta) * 100.0) / static_cast<double>(systemDelta);
}

ULONGLONG HardwareMonitor::FileTimeToUInt64(const FILETIME& ft)
{
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}
