#pragma once

#include "Common.h"

#include <functional>
#include <string>
#include <vector>

class ConsoleUI
{
public:
    using CommandHandler = std::function<void(const std::wstring&)>;

    explicit ConsoleUI(SharedState& state);
    ~ConsoleUI();

    bool Initialize(HINSTANCE instance, CommandHandler commandHandler);
    int RunMessageLoop();
    void Shutdown();

private:
    struct Rect
    {
        int x;
        int y;
        int w;
        int h;
    };

    struct HitItem
    {
        Rect rect;
        std::wstring command;
        bool openOnDoubleClick;
    };

    struct QuickPath
    {
        std::wstring label;
        std::wstring path;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    void ExecuteCommand(const std::wstring& command);
    void HandleClick(int x, int y, bool doubleClick);
    void Render(HDC targetDc);
    void BuildQuickPaths();

    void DrawChrome(HDC dc, int width, int height, const std::wstring& currentPath);
    void DrawSidebar(HDC dc, const Rect& r, const std::wstring& currentPath);
    void DrawFiles(HDC dc, const Rect& r, const std::vector<FileEntry>& files, const std::wstring& currentPath);
    void DrawRightDashboard(HDC dc, const Rect& r, const std::deque<FileChangeEvent>& changes, const HardwareSnapshot& snapshot);
    void DrawPanel(HDC dc, const Rect& r, COLORREF color);
    void DrawButton(HDC dc, const Rect& r, const std::wstring& text, COLORREF color);
    void DrawTextLine(HDC dc, const Rect& r, const std::wstring& text, COLORREF color, UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    void DrawBar(HDC dc, const Rect& r, int value, COLORREF fillColor);
    void DrawHistory(HDC dc, const Rect& r, const std::deque<int>& history, COLORREF color);

    bool Contains(const Rect& r, int x, int y) const;
    std::wstring CombinePath(const std::wstring& base, const std::wstring& name) const;
    std::wstring ParentCommand(const std::wstring& currentPath) const;
    std::wstring SizeToString(ULONGLONG bytes) const;
    COLORREF ColorForChange(FileChangeType type) const;

    SharedState& m_state;
    CommandHandler m_commandHandler;
    HINSTANCE m_instance;
    HWND m_hwnd;
    HFONT m_font;
    HFONT m_titleFont;
    std::vector<HitItem> m_hitItems;
    std::vector<QuickPath> m_quickPaths;
    HWND m_searchBox;
    HBRUSH m_searchBrush;
    std::wstring m_searchText;
};
