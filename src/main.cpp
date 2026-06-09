#include "../include/Common.h"
#include "../include/ConsoleUI.h"
#include "../include/FileExplorer.h"
#include "../include/HardwareMonitor.h"
#include "../include/dprint.h"

#include <shellapi.h>

#include <cwctype>
#include <string>

namespace
{
std::wstring GetStartupDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    DWORD length = GetCurrentDirectoryW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH)
    {
        return L".";
    }
    return buffer;
}

std::wstring ToLower(std::wstring value)
{
    for (wchar_t& ch : value)
    {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return value;
}

void PushSystemMessage(SharedState& state, const std::wstring& message, FileChangeType type)
{
    ScopedCriticalSection lock(&state.cs);
    FileChangeEvent event;
    event.fileName = message;
    event.type = type;
    event.tick = GetTickCount64();
    state.changes.push_front(event);
    while (state.changes.size() > 200)
    {
        state.changes.pop_back();
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    CDPrint::Instance().Initialize();

    SharedState state;
    FileExplorer explorer(state);
    HardwareMonitor hardware(state);
    ConsoleUI ui(state);

    std::wstring startupDirectory = GetStartupDirectory();
    if (!explorer.Start(startupDirectory))
    {
        PushSystemMessage(state, L"Failed to start file watcher", FileChangeType::Unknown);
    }

    if (!hardware.Start())
    {
        PushSystemMessage(state, L"Failed to start hardware monitor", FileChangeType::Unknown);
    }

    auto commandHandler = [&](const std::wstring& command) {
        std::wstring lowered = ToLower(command);
        if (lowered == L"exit" || lowered == L"quit")
        {
            state.running.store(false);
            PostQuitMessage(0);
        }
        else if (lowered.rfind(L"cd", 0) == 0)
        {
            if (!explorer.ChangeDirectory(command))
            {
                PushSystemMessage(state, L"Invalid directory: " + command, FileChangeType::Unknown);
            }
        }
        else if (lowered.rfind(L"open ", 0) == 0)
        {
            std::wstring target = command.substr(5);
            HINSTANCE result = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(result) <= 32)
            {
                PushSystemMessage(state, L"Cannot open: " + target, FileChangeType::Unknown);
            }
        }
        else if (lowered == L"refresh")
        {
            explorer.ScanCurrentDirectory();
        }
        else if (!command.empty())
        {
            PushSystemMessage(state, L"Unknown command: " + command, FileChangeType::Unknown);
        }
    };

    if (!ui.Initialize(instance, commandHandler))
    {
        DPRINT_ERROR(L"ConsoleUI initialization failed");
        hardware.Stop();
        explorer.Stop();
        CDPrint::Instance().Shutdown();
        return 1;
    }

    int result = ui.RunMessageLoop();

    hardware.Stop();
    explorer.Stop();
    ui.Shutdown();
    CDPrint::Instance().Shutdown();
    return result;
}
