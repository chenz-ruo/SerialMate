#include "ExtensionControlIds.h"
#include "Product.h"
#include "UiGeometry.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
constexpr UINT kQuery = WM_APP + 100;
constexpr int kIntervalEditId = 121;
int checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

LRESULT Message(HWND window, UINT message, WPARAM wParam = 0, LPARAM lParam = 0) {
    DWORD_PTR result = 0;
    Check(SendMessageTimeoutW(window, message, wParam, lParam, SMTO_ABORTIFHUNG, 2000, &result) != 0,
          "window message timed out");
    return static_cast<LRESULT>(result);
}

std::wstring Text(HWND window) {
    const auto length = Message(window, WM_GETTEXTLENGTH);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    text.resize(static_cast<std::size_t>(Message(window, WM_GETTEXT, text.size(),
                                                reinterpret_cast<LPARAM>(text.data()))));
    return text;
}

struct TestApp {
    PROCESS_INFORMATION process{};
    HWND window = nullptr;
    ~TestApp() {
        if (IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
        if (process.hProcess) {
            // Only the process created by this test can be terminated on failure.
            if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
                TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
    }
    static BOOL CALLBACK Find(HWND candidate, LPARAM parameter) {
        auto& app = *reinterpret_cast<TestApp*>(parameter);
        DWORD pid = 0;
        GetWindowThreadProcessId(candidate, &pid);
        wchar_t className[128]{};
        GetClassNameW(candidate, className, static_cast<int>(std::size(className)));
        if (pid == app.process.dwProcessId && wcscmp(className, product::kMainWindowClass) == 0) {
            app.window = candidate;
            return FALSE;
        }
        return TRUE;
    }
    void Start(const std::wstring& path) {
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_SHOWNOACTIVATE;
        auto command = L"\"" + path + L"\"";
        Check(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             nullptr, &startup, &process) != FALSE, "cannot launch newly built EXE");
        WaitForInputIdle(process.hProcess, 5000);
        for (int i = 0; i < 100 && !window; ++i) {
            EnumWindows(Find, reinterpret_cast<LPARAM>(this));
            if (!window) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        Check(window != nullptr, "main window was not created");
        Message(window, WM_NULL);
    }
};

RECT ChildRect(HWND parent, HWND child) {
    RECT rect{};
    Check(GetWindowRect(child, &rect) != FALSE, "missing child rectangle");
    MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

void CheckControl(HWND parent, int id, const RECT& expected, bool visible) {
    HWND child = GetDlgItem(parent, id);
    Check(child != nullptr, "extension control is missing");
    Check((IsWindowVisible(child) != FALSE) == visible, "extension control visibility mismatch");
    if (visible) {
        const auto actual = ChildRect(parent, child);
        Check(EqualRect(&actual, &expected) != FALSE, "actual child rectangle differs from MainLayoutGeometry");
    }
}

void CheckFlatEdit(HWND edit, const char* message) {
    Check(edit != nullptr, "flat edit control is missing");
    Check((GetWindowLongPtrW(edit, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) == 0, message);
}

void CheckWindow(HWND window, const MainLayoutGeometry& geometry, bool report) {
    const auto query = [&](WPARAM key) { return Message(window, kQuery, key); };
    Check(query(40) == (geometry.extensionVisible ? 1 : 0) && query(49) == static_cast<LRESULT>(geometry.phase),
          "actual width phase mismatch");
    Check(query(41) == geometry.extensionStartWidth && query(42) == geometry.extensionFullVisibleWidth &&
          query(43) == geometry.extensionColumnWidth && query(44) == geometry.visibleCustomRows,
          "actual expansion budget, column width or visible row count mismatch");
    Check(query(50) == geometry.defaultClientWidth && query(52) == geometry.leftColumnWidth &&
          query(53) == geometry.mainColumnMinWidth8 && query(54) == geometry.extensionFullWidth &&
          query(55) == geometry.extensionGap, "actual width budget metrics mismatch");
    Check(query(26) == geometry.commRecordCard.top && query(27) == geometry.commRecordCard.bottom &&
          query(28) == geometry.dataSendCard.top && query(29) == geometry.dataSendCard.bottom &&
          query(30) == geometry.mainCardGap, "main card geometry mismatch");
    Check(query(27) == query(23), "actual communication bottom does not align with receive settings");
    Check(query(28) == query(24), "actual data send top does not align with send settings");
    CheckControl(window, 100, geometry.openButton, true);
    CheckControl(window, 101, geometry.closeButton, true);
    CheckControl(window, 102, geometry.newButton, true);
    CheckControl(window, 103, geometry.logButton, true);
    CheckControl(window, 104, geometry.aboutButton, true);
    Check(geometry.openButton.left == geometry.settings.serialCard.left &&
          geometry.closeButton.right == geometry.settings.serialCard.right &&
          geometry.aboutButton.right == geometry.currentVisibleContentRight,
          "toolbar bounds align with the visible card geometry");
    Check(query(45) == geometry.customDataCard.top && query(46) == geometry.customDataCard.bottom &&
          query(47) == geometry.protocolCard.top && query(48) == geometry.protocolCard.bottom,
          "extension card geometry mismatch");
    if (geometry.extensionVisible)
        Check(query(47) == query(24), "actual protocol top does not align with send settings");
    CheckControl(window, extension::CustomTitle, geometry.customDataTitle, !IsRectEmpty(&geometry.customDataTitle));
    CheckControl(window, extension::ProtocolTitle, geometry.protocolTitle, !IsRectEmpty(&geometry.protocolTitle));
    for (int i = 0; i < kMaximumStoredCustomSlots; ++i) {
        const auto& row = geometry.customSlots[static_cast<std::size_t>(i)];
        const bool visible = i < geometry.visibleCustomRows;
        CheckControl(window, extension::IndexFirst + i, row.index, visible);
        CheckControl(window, extension::EditFirst + i, row.edit, visible);
        CheckControl(window, extension::SendFirst + i, row.send, visible);
    }
    const auto bytes = query(9);
    Check(bytes == 8 || bytes == 16 || bytes == 32, "bytes per row left the 8/16/32 candidates");
    if (geometry.rightWidth == geometry.mainColumnMinWidth8)
        Check(bytes == 8, "the base and expansion phases must naturally display eight bytes");
    RECT lastTool{};
    const auto title = ChildRect(window, GetDlgItem(window, 9004));
    const auto recordView = ChildRect(window, GetDlgItem(window, 127));
    for (int id : {116, 125, 123, 124, 126}) {
        const auto tool = ChildRect(window, GetDlgItem(window, id));
        RECT overlap{};
        Check(tool.left >= geometry.commRecordCard.left && tool.right <= geometry.commRecordCard.right &&
              tool.top >= geometry.commRecordCard.top && tool.bottom <= recordView.top,
              "compact communication toolbar escaped its card or covered data");
        Check(!IntersectRect(&overlap, &tool, &title) && !IntersectRect(&overlap, &tool, &lastTool),
              "toolbar overlaps another button or the title");
        lastTool = tool;
    }
    if (report) {
        RECT client{}; GetClientRect(window, &client);
        std::cout << "ClientWidth=" << client.right << " ClientHeight=" << client.bottom
                  << " DPI=" << GetDpiForWindow(window) << " WidthPhase=" << query(49)
                  << " LeftColumnWidth=" << query(52) << " MainWidth=" << geometry.rightWidth
                  << " MainMin8=" << query(53) << " DefaultClientWidth=" << query(50)
                  << " ExtensionStartWidth=" << query(41) << " ExtensionFullVisibleWidth=" << query(42)
                  << " ExtensionGap=" << query(55) << " ExtensionColumnWidth=" << query(43)
                  << " VisibleCustomRows=" << query(44) << " BytesPerRow=" << bytes << '\n';
        const auto print = [](const char* name, const RECT& r) {
            std::cout << name << "=[" << r.left << ',' << r.top << ',' << r.right << ',' << r.bottom << "]\n";
        };
        print("commRecordCard", geometry.commRecordCard);
        print("dataSendCard", geometry.dataSendCard);
        print("customDataCard", geometry.customDataCard);
        print("protocolCard", geometry.protocolCard);
    }
}

void Run(HWND window) {
    RECT startupClient{}, startupWindow{};
    GetClientRect(window, &startupClient); GetWindowRect(window, &startupWindow);
    const int defaultWidth = static_cast<int>(Message(window, kQuery, 50));
    const int fullWidth = static_cast<int>(Message(window, kQuery, 42));
    Check(startupClient.right == defaultWidth && Message(window, kQuery, 43) == 0 &&
          Message(window, kQuery, 9) == 8, "initial shown window must start in Phase A with eight bytes");
    std::cout << "DefaultWindowWidth=" << startupWindow.right - startupWindow.left << '\n';
    std::cout << "DPI=" << Message(window, kQuery, 57)
              << " LeftFixedWidthLogical=" << kLeftColumnWidthLogical
              << " LeftFixedWidthPhysical=" << Message(window, kQuery, 58)
              << " SerialCardPhysicalWidth=" << Message(window, kQuery, 58)
              << " ReceiveCardPhysicalWidth=" << Message(window, kQuery, 59)
              << " SendCardPhysicalWidth=" << Message(window, kQuery, 60)
              << " OpenButtonLeft=" << Message(window, kQuery, 61)
              << " CloseButtonRight=" << Message(window, kQuery, 62)
              << " CommLeft=" << Message(window, kQuery, 63)
              << " CommWidth=" << Message(window, kQuery, 64) << '\n';
    // Populate only the test instance's display buffer, without opening hardware.
    // Subsequent width checks therefore include an active native scrollbar.
    std::wstring recordProbe(257, L'X');
    COPYDATASTRUCT probe{6, static_cast<DWORD>((recordProbe.size() + 1) * sizeof(wchar_t)), recordProbe.data()};
    Message(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&probe));
    std::array<std::array<HWND, 3>, kMaximumStoredCustomSlots> handles{};
    std::set<HWND> unique;
    CheckFlatEdit(GetDlgItem(window, kIntervalEditId),
                  "timed-send interval edit retained native client edge");
    const std::array<int, 3> firstIds{extension::IndexFirst, extension::EditFirst, extension::SendFirst};
    const std::array<const wchar_t*, 3> classes{L"Static", L"Edit", L"Button"};
    for (int i = 0; i < kMaximumStoredCustomSlots; ++i) {
        for (std::size_t column = 0; column < firstIds.size(); ++column) {
            HWND child = GetDlgItem(window, firstIds[column] + i);
            wchar_t className[64]{};
            Check(child && GetClassNameW(child, className, static_cast<int>(std::size(className))) &&
                  _wcsicmp(className, classes[column]) == 0, "slot ID resolves to wrong control type");
            Check(unique.insert(child).second, "two slots share a control handle");
            handles[static_cast<std::size_t>(i)][column] = child;
        }
        CheckFlatEdit(handles[static_cast<std::size_t>(i)][1],
                      "custom data edit retained native client edge");
        Check(Text(handles[static_cast<std::size_t>(i)][0]) == std::to_wstring(i + 1), "slot number mismatch");
        const auto sentinel = L"槽位 " + std::to_wstring(i + 1) + L" / preserved data";
        Message(handles[static_cast<std::size_t>(i)][1], WM_SETTEXT, 0, reinterpret_cast<LPARAM>(sentinel.c_str()));
    }
    Check(unique.size() == 48, "all 16 slots must exist before any resize");
    Check(Text(GetDlgItem(window, extension::CustomTitle)) == L"自定义数据" &&
          Text(GetDlgItem(window, extension::ProtocolTitle)) == L"协议数据生成", "card titles mismatch");
    const auto titleFont = Message(GetDlgItem(window, 9004), WM_GETFONT);
    Check(Message(GetDlgItem(window, extension::CustomTitle), WM_GETFONT) == titleFont &&
          Message(GetDlgItem(window, extension::ProtocolTitle), WM_GETFONT) == titleFont,
          "extension titles do not use the shared card font");

    const auto resize = [&](int width, int height, bool report = false) {
        RECT outer{}, client{};
        GetWindowRect(window, &outer); GetClientRect(window, &client);
        Check(SetWindowPos(window, nullptr, outer.left, outer.top,
              width + outer.right - outer.left - client.right,
              height + outer.bottom - outer.top - client.bottom,
              // Exercise ultra-wide geometry even when it exceeds this monitor's tracking limit.
              SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING) != FALSE, "cannot resize test window");
        Message(window, WM_NULL);
        GetClientRect(window, &client);
        if (client.right != width || client.bottom != height)
            std::cerr << "RequestedClient=" << width << 'x' << height << " ActualClient="
                      << client.right << 'x' << client.bottom << '\n';
        Check(client.right == width && client.bottom == height, "requested client dimensions were not applied");
        const auto geometry = CalculateMainLayoutGeometry(width, height, GetDpiForWindow(window),
                                                           static_cast<int>(Message(window, kQuery, 51)));
        CheckWindow(window, geometry, report);
        for (int i = 0; i < kMaximumStoredCustomSlots; ++i) {
            for (std::size_t column = 0; column < firstIds.size(); ++column)
                Check(GetDlgItem(window, firstIds[column] + i) == handles[static_cast<std::size_t>(i)][column],
                      "slot control was recreated during resize");
            const auto sentinel = L"槽位 " + std::to_wstring(i + 1) + L" / preserved data";
            Check(Text(GetDlgItem(window, extension::EditFirst + i)) == sentinel, "hidden slot lost its data");
        }
        return geometry;
    };
    const auto base = resize(defaultWidth, 1024, true);
    const std::array<int, 9> widths{defaultWidth + 1, defaultWidth + 80, defaultWidth + 100,
        defaultWidth + 160, fullWidth, fullWidth + 100, fullWidth + 200, fullWidth + 300, fullWidth + 600};
    for (int width : widths) {
        const auto geometry = resize(width, 1024, true);
        Check(geometry.leftColumnWidth == base.leftColumnWidth, "left column grew during horizontal resize");
        if (width <= fullWidth)
            Check(geometry.rightWidth == base.mainColumnMinWidth8, "main width changed during extension expansion");
        else
            Check(geometry.extensionColumnWidth == base.extensionFullWidth &&
                  geometry.rightWidth == base.mainColumnMinWidth8 + width - fullWidth,
                  "fully expanded side column stole main width");
    }
    resize(fullWidth + MulDiv(1600, static_cast<int>(GetDpiForWindow(window)), 96), 1024, true);
    Check(Message(window, kQuery, 9) == 32, "ultra-wide main column did not reach 32 bytes");
    for (int height : {768, 900, 1024, 1280, 1600, 768}) resize(fullWidth, height);
    for (int delta = 0; delta <= 24; ++delta) {
        const auto geometry = resize(defaultWidth + delta, 1024);
        Check(geometry.rightWidth == base.mainColumnMinWidth8, "gap appeared by shrinking the main column");
    }
    for (int delta = 24; delta >= 0; --delta) resize(defaultWidth + delta, 1024);
    for (int delta : {-2, -1, 0, 1, 2, 1, 0, -1, -2}) resize(fullWidth + delta, 1024);
    ShowWindow(window, SW_MAXIMIZE);
    Message(window, WM_NULL);
    RECT maximum{}; GetClientRect(window, &maximum);
    std::cout << "MaxWindow:\n";
    CheckWindow(window, CalculateMainLayoutGeometry(maximum.right, maximum.bottom, GetDpiForWindow(window),
                                                     static_cast<int>(Message(window, kQuery, 51))), true);
    ShowWindow(window, SW_RESTORE);
    Message(window, WM_NULL);
    resize(defaultWidth, 1024, true);
    Check(Message(window, kQuery, 0) == 0 && Message(window, kQuery, 1) == 0 &&
          Message(window, kQuery, 2) == 0, "layout test must not open or transmit on a serial port");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::cerr << "Usage: ResponsiveUiTests SerialMate.exe\n"; return 2; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    TestApp app;
    try {
        app.Start(std::filesystem::absolute(argv[1]).wstring());
        Run(app.window);
        PostMessageW(app.window, WM_CLOSE, 0, 0);
        Check(WaitForSingleObject(app.process.hProcess, 5000) == WAIT_OBJECT_0, "test app failed to close");
        DWORD exitCode = 1;
        Check(GetExitCodeProcess(app.process.hProcess, &exitCode) && exitCode == 0, "test app exit was not clean");
        std::cout << "NativeCustomRowGeometryTests=Passed\nNativeContinuousWidthTests=Passed\n"
                     "Stable16SlotHandlesAndContents=Passed\nOrphanControlTests=Passed\n"
                  << "Checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << " (check " << checks << ")\n";
        return 1;
    }
}
