#include "../include/ConsoleUI.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace
{
constexpr wchar_t kWindowClassName[] = L"ExplorerMonitorGuiWindow";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr int kTopHeight = 112;
constexpr int kSidebarWidth = 218;
constexpr int kDashboardWidth = 330;
constexpr int kPadding = 12;

COLORREF Rgb(BYTE r, BYTE g, BYTE b)
{
    return RGB(r, g, b);
}

std::wstring Percent(double value)
{
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(1) << value << L"%";
    return ss.str();
}

std::wstring EnvPath(const wchar_t* name, const wchar_t* suffix)
{
    wchar_t value[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(name, value, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        return L"";
    }
    std::wstring path = value;
    if (suffix && suffix[0] != L'\0')
    {
        path += suffix;
    }
    return path;
}
}

ConsoleUI::ConsoleUI(SharedState& state)
    : m_state(state),
      m_instance(nullptr),
      m_hwnd(nullptr),
      m_font(nullptr),
      m_titleFont(nullptr),
      m_searchBox(nullptr),
      m_searchBrush(nullptr)
{
}

ConsoleUI::~ConsoleUI()
{
    Shutdown();
}

bool ConsoleUI::Initialize(HINSTANCE instance, CommandHandler commandHandler)
{
    m_instance = instance;
    m_commandHandler = std::move(commandHandler);
    BuildQuickPaths();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = m_instance;
    wc.lpfnWndProc = &ConsoleUI::WindowProc;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    RegisterClassExW(&wc);

    m_font = CreateFontW(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    m_titleFont = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    m_hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        L"File Explorer Extended",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1220,
        760,
        nullptr,
        nullptr,
        m_instance,
        this);

    if (!m_hwnd)
    {
        return false;
    }

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

int ConsoleUI::RunMessageLoop()
{
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void ConsoleUI::Shutdown()
{
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_font)
    {
        DeleteObject(m_font);
        m_font = nullptr;
    }
    if (m_titleFont)
    {
        DeleteObject(m_titleFont);
        m_titleFont = nullptr;
    }
    if (m_searchBrush)
    {
        DeleteObject(m_searchBrush);
        m_searchBrush = nullptr;
    }
}

LRESULT CALLBACK ConsoleUI::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    ConsoleUI* ui = nullptr;
    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        ui = static_cast<ConsoleUI*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
    }
    else
    {
        ui = reinterpret_cast<ConsoleUI*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (ui)
    {
        return ui->HandleMessage(hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT ConsoleUI::HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_CREATE:
        SetTimer(hwnd, kRefreshTimer, 100, nullptr);
        m_searchBox = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 100, 30,
            hwnd, reinterpret_cast<HMENU>(101), m_instance, nullptr);
        m_searchBrush = CreateSolidBrush(Rgb(45, 45, 45));
        SendMessageW(m_searchBox, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
        return 0;
    case WM_SIZE:
    {
        RECT client{};
        GetClientRect(hwnd, &client);
        if (m_searchBox)
            SetWindowPos(m_searchBox, nullptr,
                client.right - 220, 57, 198, 28, SWP_NOZORDER);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == EN_CHANGE && LOWORD(wparam) == 101)
        {
            wchar_t buf[512]{};
            GetWindowTextW(m_searchBox, buf, 512);
            m_searchText = buf;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CTLCOLOREDIT:
    {
        HDC hdc = reinterpret_cast<HDC>(wparam);
        SetTextColor(hdc, Rgb(230, 230, 230));
        SetBkColor(hdc, Rgb(45, 45, 45));
        return reinterpret_cast<LRESULT>(m_searchBrush);
    }
    case WM_LBUTTONDOWN:
        HandleClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), false);
        return 0;
    case WM_LBUTTONDBLCLK:
        HandleClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), true);
        return 0;
    case WM_TIMER:
        if (wparam == kRefreshTimer)
        {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC windowDc = BeginPaint(hwnd, &ps);
        Render(windowDc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        m_state.running.store(false);
        KillTimer(hwnd, kRefreshTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void ConsoleUI::ExecuteCommand(const std::wstring& command)
{
    if (m_commandHandler)
    {
        m_commandHandler(command);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void ConsoleUI::HandleClick(int x, int y, bool doubleClick)
{
    for (const HitItem& item : m_hitItems)
    {
        if (Contains(item.rect, x, y))
        {
            if (!item.openOnDoubleClick || doubleClick)
            {
                ExecuteCommand(item.command);
            }
            return;
        }
    }
}

void ConsoleUI::Render(HDC targetDc)
{
    RECT client{};
    GetClientRect(m_hwnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    HDC memoryDc = CreateCompatibleDC(targetDc);
    HBITMAP bitmap = CreateCompatibleBitmap(targetDc, width, height);
    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);

    HBRUSH background = CreateSolidBrush(Rgb(18, 18, 18));
    FillRect(memoryDc, &client, background);
    DeleteObject(background);

    SelectObject(memoryDc, m_font);
    SetBkMode(memoryDc, TRANSPARENT);
    m_hitItems.clear();

    std::vector<FileEntry> files;
    std::deque<FileChangeEvent> changes;
    HardwareSnapshot hardware;
    std::wstring currentPath;
    {
        ScopedCriticalSection lock(&m_state.cs);
        // UI는 파일명/크기/종류만 그리므로 무거운 lines(파일 내용)는 복사 X
        // 100ms 타이머마다 전체 파일 내용을 복사하던 부하를 없애 화면 갱신을 가볍게 (검색창 깜빡임 방지)
        files.reserve(m_state.files.size());
        for (const FileEntry& src : m_state.files)
        {
            FileEntry e;
            e.name = src.name;
            e.isDirectory = src.isDirectory;
            e.sizeBytes = src.sizeBytes;
            e.lastWriteTime = src.lastWriteTime;
            e.lineCount = src.lineCount;
            // e.lines 는 일부러 복사하지 않음 (UI 미사용)
            files.push_back(std::move(e));
        }
        changes = m_state.changes;
        hardware = m_state.hardware;
        currentPath = m_state.currentPath;
    }

    DrawChrome(memoryDc, width, height, currentPath);

    Rect sidebar{0, kTopHeight, kSidebarWidth, height - kTopHeight};
    Rect filesRect{kSidebarWidth, kTopHeight, std::max(260, width - kSidebarWidth - kDashboardWidth), height - kTopHeight};
    Rect dashboard{width - kDashboardWidth, kTopHeight, kDashboardWidth, height - kTopHeight};

    DrawSidebar(memoryDc, sidebar, currentPath);
    DrawFiles(memoryDc, filesRect, files, currentPath);
    DrawRightDashboard(memoryDc, dashboard, changes, hardware);

    BitBlt(targetDc, 0, 0, width, height, memoryDc, 0, 0, SRCCOPY);

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
}

void ConsoleUI::BuildQuickPaths()
{
    m_quickPaths.clear();
    std::wstring profile = EnvPath(L"USERPROFILE", L"");
    if (!profile.empty())
    {
        m_quickPaths.push_back({L"Desktop", profile + L"\\Desktop"});
        m_quickPaths.push_back({L"Downloads", profile + L"\\Downloads"});
        m_quickPaths.push_back({L"Documents", profile + L"\\Documents"});
        m_quickPaths.push_back({L"Pictures", profile + L"\\Pictures"});
    }
    wchar_t current[MAX_PATH]{};
    if (GetCurrentDirectoryW(MAX_PATH, current) > 0)
    {
        m_quickPaths.push_back({L"Project Folder", current});
    }
}

void ConsoleUI::DrawChrome(HDC dc, int width, int, const std::wstring& currentPath)
{
    RECT top{0, 0, width, kTopHeight};
    HBRUSH topBrush = CreateSolidBrush(Rgb(31, 31, 31));
    FillRect(dc, &top, topBrush);
    DeleteObject(topBrush);

    DrawTextLine(dc, Rect{18, 12, 180, 28}, L"File Explorer+", Rgb(240, 240, 240));

    Rect back{18, 54, 38, 34};
    Rect up{64, 54, 38, 34};
    Rect refresh{110, 54, 82, 34};
    DrawButton(dc, back, L"<", Rgb(47, 47, 47));
    DrawButton(dc, up, L"Up", Rgb(47, 47, 47));
    DrawButton(dc, refresh, L"Refresh", Rgb(47, 47, 47));
    m_hitItems.push_back({back, ParentCommand(currentPath), false});
    m_hitItems.push_back({up, ParentCommand(currentPath), false});
    m_hitItems.push_back({refresh, L"refresh", false});

    Rect address{206, 54, std::max(240, width - 440), 34};
    DrawPanel(dc, address, Rgb(45, 45, 45));
    DrawTextLine(dc, Rect{address.x + 14, address.y, address.w - 28, address.h}, currentPath, Rgb(230, 230, 230));

    Rect search{width - 220, 54, 202, 34};
}

void ConsoleUI::DrawSidebar(HDC dc, const Rect& r, const std::wstring&)
{
    RECT side{r.x, r.y, r.x + r.w, r.y + r.h};
    HBRUSH sideBrush = CreateSolidBrush(Rgb(25, 25, 25));
    FillRect(dc, &side, sideBrush);
    DeleteObject(sideBrush);

    SelectObject(dc, m_titleFont);
    DrawTextLine(dc, Rect{18, r.y + 14, r.w - 30, 24}, L"Quick Access", Rgb(220, 220, 220));
    SelectObject(dc, m_font);

    int y = r.y + 52;
    for (const QuickPath& quick : m_quickPaths)
    {
        if (quick.path.empty())
        {
            continue;
        }
        Rect item{12, y, r.w - 24, 34};
        DrawButton(dc, item, quick.label, Rgb(34, 34, 34));
        m_hitItems.push_back({item, L"cd " + quick.path, false});
        y += 40;
    }
}

void ConsoleUI::DrawFiles(HDC dc, const Rect& r, const std::vector<FileEntry>& files, const std::wstring& currentPath)
{
    RECT content{r.x, r.y, r.x + r.w, r.y + r.h};
    HBRUSH brush = CreateSolidBrush(Rgb(18, 18, 18));
    FillRect(dc, &content, brush);
    DeleteObject(brush);

    SelectObject(dc, m_titleFont);
    DrawTextLine(dc, Rect{r.x + 22, r.y + 14, r.w - 44, 28}, L"Name", Rgb(230, 230, 230));
    SelectObject(dc, m_font);
    DrawTextLine(dc, Rect{r.x + r.w - 150, r.y + 14, 120, 28}, L"Size", Rgb(170, 170, 170));

    int y = r.y + 52;
    int rowHeight = 34;
    int rows = std::max(0, (r.h - 62) / rowHeight);
    std::vector<const FileEntry*> filtered;
    for (const FileEntry& f : files)
    {
        if (m_searchText.empty())
        {
            filtered.push_back(&f);
        }
        else
        {
            std::wstring nameLower = f.name;
            std::wstring searchLower = m_searchText;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);
            std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::towlower);
            if (nameLower.find(searchLower) != std::wstring::npos)
                filtered.push_back(&f);
        }
    }

    for (int i = 0; i < rows && i < static_cast<int>(filtered.size()); ++i)
    {
        const FileEntry& entry = *filtered[i];
        Rect row{r.x + 12, y + i * rowHeight, r.w - 24, rowHeight - 2};
        if (i % 2 == 0)
        {
            DrawPanel(dc, row, Rgb(23, 23, 23));
        }

        std::wstring icon = entry.isDirectory ? L"[Folder] " : L"[File]   ";
        DrawTextLine(dc, Rect{row.x + 12, row.y, row.w - 170, row.h}, icon + entry.name,
            entry.isDirectory ? Rgb(255, 216, 111) : Rgb(232, 232, 232));
        DrawTextLine(dc, Rect{row.x + row.w - 135, row.y, 120, row.h}, entry.isDirectory ? L"" : SizeToString(entry.sizeBytes), Rgb(160, 160, 160), DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

        if (entry.isDirectory)
        {
            m_hitItems.push_back({row, L"cd " + CombinePath(currentPath, entry.name), true});
        }
        else
        {
            m_hitItems.push_back({row, L"open " + CombinePath(currentPath, entry.name), true});
        }
    }
}

void ConsoleUI::DrawRightDashboard(HDC dc, const Rect& r, const std::deque<FileChangeEvent>& changes, const HardwareSnapshot& snapshot)
{
    RECT right{r.x, r.y, r.x + r.w, r.y + r.h};
    HBRUSH brush = CreateSolidBrush(Rgb(22, 22, 22));
    FillRect(dc, &right, brush);
    DeleteObject(brush);

    SelectObject(dc, m_titleFont);
    DrawTextLine(dc, Rect{r.x + 16, r.y + 14, r.w - 32, 26}, L"Live Changes", Rgb(230, 230, 230));
    SelectObject(dc, m_font);

    int y = r.y + 48;
    for (int i = 0; i < 8 && i < static_cast<int>(changes.size()); ++i)
    {
        const FileChangeEvent& event = changes[i];
        std::wstring line = ChangeTypeToString(event.type) + L"  " + event.fileName;
        if (!event.detail.empty())
        {
            line += L"  (" + event.detail + L")";  // 수정 상세가 있으면 괄호 +
        }
        DrawTextLine(dc, Rect{ r.x + 16, y + i * 24, r.w - 32, 22 }, line, ColorForChange(event.type));
    }

    int hwY = r.y + 270;
    SelectObject(dc, m_titleFont);
    DrawTextLine(dc, Rect{r.x + 16, hwY, r.w - 32, 26}, L"Hardware", Rgb(230, 230, 230));
    SelectObject(dc, m_font);

    DrawTextLine(dc, Rect{r.x + 16, hwY + 38, r.w - 32, 22}, L"CPU  " + Percent(snapshot.cpuUsage), Rgb(242, 190, 94));
    DrawBar(dc, Rect{r.x + 16, hwY + 66, r.w - 32, 14}, static_cast<int>(snapshot.cpuUsage), Rgb(242, 190, 94));

    std::wstringstream mem;
    mem << L"RAM  " << snapshot.memoryLoad << L"%  " << snapshot.usedMemoryMb << L"/" << snapshot.totalMemoryMb << L" MB";
    DrawTextLine(dc, Rect{r.x + 16, hwY + 94, r.w - 32, 22}, mem.str(), Rgb(139, 198, 255));
    DrawBar(dc, Rect{r.x + 16, hwY + 122, r.w - 32, 14}, static_cast<int>(snapshot.memoryLoad), Rgb(139, 198, 255));
    DrawHistory(dc, Rect{r.x + 16, hwY + 160, r.w - 32, 72}, snapshot.cpuHistory, Rgb(242, 190, 94));
}

void ConsoleUI::DrawPanel(HDC dc, const Rect& r, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, Rgb(55, 55, 55));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.x, r.y, r.x + r.w, r.y + r.h, 7, 7);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void ConsoleUI::DrawButton(HDC dc, const Rect& r, const std::wstring& text, COLORREF color)
{
    DrawPanel(dc, r, color);
    DrawTextLine(dc, Rect{r.x + 10, r.y, r.w - 20, r.h}, text, Rgb(230, 230, 230), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

void ConsoleUI::DrawTextLine(HDC dc, const Rect& r, const std::wstring& text, COLORREF color, UINT format)
{
    RECT rect{r.x, r.y, r.x + r.w, r.y + r.h};
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | DT_END_ELLIPSIS);
}

void ConsoleUI::DrawBar(HDC dc, const Rect& r, int value, COLORREF fillColor)
{
    value = std::clamp(value, 0, 100);
    HBRUSH backBrush = CreateSolidBrush(Rgb(44, 44, 44));
    RECT back{r.x, r.y, r.x + r.w, r.y + r.h};
    FillRect(dc, &back, backBrush);
    DeleteObject(backBrush);

    HBRUSH fillBrush = CreateSolidBrush(fillColor);
    RECT fill{r.x, r.y, r.x + (r.w * value / 100), r.y + r.h};
    FillRect(dc, &fill, fillBrush);
    DeleteObject(fillBrush);
}

void ConsoleUI::DrawHistory(HDC dc, const Rect& r, const std::deque<int>& history, COLORREF color)
{
    HPEN framePen = CreatePen(PS_SOLID, 1, Rgb(60, 60, 60));
    HGDIOBJ oldPen = SelectObject(dc, framePen);
    Rectangle(dc, r.x, r.y, r.x + r.w, r.y + r.h);
    SelectObject(dc, oldPen);
    DeleteObject(framePen);

    HPEN pen = CreatePen(PS_SOLID, 2, color);
    oldPen = SelectObject(dc, pen);
    if (history.size() >= 2)
    {
        int count = static_cast<int>(history.size());
        for (int i = 1; i < count; ++i)
        {
            int x1 = r.x + ((i - 1) * r.w) / std::max(1, count - 1);
            int x2 = r.x + (i * r.w) / std::max(1, count - 1);
            int y1 = r.y + r.h - (std::clamp(history[i - 1], 0, 100) * r.h / 100);
            int y2 = r.y + r.h - (std::clamp(history[i], 0, 100) * r.h / 100);
            MoveToEx(dc, x1, y1, nullptr);
            LineTo(dc, x2, y2);
        }
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

bool ConsoleUI::Contains(const Rect& r, int x, int y) const
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

std::wstring ConsoleUI::CombinePath(const std::wstring& base, const std::wstring& name) const
{
    if (base.empty() || base.back() == L'\\')
    {
        return base + name;
    }
    return base + L"\\" + name;
}

std::wstring ConsoleUI::ParentCommand(const std::wstring& currentPath) const
{
    if (currentPath.empty())
    {
        return L"refresh";
    }
    return L"cd " + CombinePath(currentPath, L"..");
}

std::wstring ConsoleUI::SizeToString(ULONGLONG bytes) const
{
    std::wstringstream ss;
    if (bytes >= 1024ull * 1024ull * 1024ull)
    {
        ss << (bytes / (1024ull * 1024ull * 1024ull)) << L"GB";
    }
    else if (bytes >= 1024ull * 1024ull)
    {
        ss << (bytes / (1024ull * 1024ull)) << L"MB";
    }
    else if (bytes >= 1024ull)
    {
        ss << (bytes / 1024ull) << L"KB";
    }
    else
    {
        ss << bytes << L"B";
    }
    return ss.str();
}

COLORREF ConsoleUI::ColorForChange(FileChangeType type) const
{
    switch (type)
    {
    case FileChangeType::Added:
        return Rgb(111, 207, 151);
    case FileChangeType::Removed:
        return Rgb(255, 118, 118);
    case FileChangeType::Modified:
        return Rgb(242, 190, 94);
    case FileChangeType::RenamedOld:
    case FileChangeType::RenamedNew:
        return Rgb(139, 198, 255);
    default:
        return Rgb(220, 220, 220);
    }
}
