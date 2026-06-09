#pragma once

#include "Common.h"

class HardwareMonitor
{
public:
    explicit HardwareMonitor(SharedState& state);
    ~HardwareMonitor();

    bool Start();
    void Stop();

private:
    static DWORD WINAPI ThreadThunk(LPVOID param);
    DWORD PollLoop();
    double CalculateCpuUsage(const FILETIME& idle, const FILETIME& kernel, const FILETIME& user);
    static ULONGLONG FileTimeToUInt64(const FILETIME& ft);

    SharedState& m_state;
    HANDLE m_threadHandle;
    HANDLE m_stopEvent;
    FILETIME m_prevIdle;
    FILETIME m_prevKernel;
    FILETIME m_prevUser;
    bool m_hasPreviousCpuSample;
};
