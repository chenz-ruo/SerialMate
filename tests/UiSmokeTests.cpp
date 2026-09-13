#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "Version.h"

namespace {

constexpr wchar_t kWindowClass[] = L"SerialMateMainWindow";
constexpr UINT WM_TEST_QUERY = WM_APP + 100;
enum ControlId : int {
    ID_OPEN = 100, ID_CLOSE, ID_NEW, ID_LOG, ID_ABOUT,
    ID_PORT, ID_BAUD, ID_DATA_BITS, ID_STOP_BITS, ID_PARITY, ID_FLOW,
    ID_TIMESTAMP, ID_AUTOLINE, ID_SIMPLE_MODE, ID_RX_ONLY, ID_RX_HEX, ID_RX_FILE,
    ID_TX_HEX, ID_TX_CR, ID_TX_LF, ID_TIMED, ID_INTERVAL, ID_SEND_TIMED,
    ID_COPY_ALL, ID_CLEAR_LOG, ID_PAUSE, ID_EXPORT, ID_LOG_VIEW,
    ID_SEND_EDIT, ID_SEND, ID_CLEAR_SEND, ID_LOAD_FILE, ID_SEND_FILE,
    ID_STATUS_LEFT, ID_STATUS_RIGHT, ID_ENCODING
};

struct WindowSearch { DWORD processId; HWND window; };

BOOL CALLBACK FindProcessWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (processId == search->processId && wcscmp(className, kWindowClass) == 0) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindWindowForProcess(DWORD processId, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        WindowSearch search{processId, nullptr};
        EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
        if (search.window) return search.window;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);
    return nullptr;
}

bool WaitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);
    return condition();
}

RECT ChildRect(HWND main, int id) {
    RECT rect{};
    GetWindowRect(GetDlgItem(main, id), &rect);
    MapWindowPoints(HWND_DESKTOP, main, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

int CenterY(const RECT& rect) { return (rect.top + rect.bottom) / 2; }

bool SettingsControlsHaveValidGeometry(HWND main) {
    const RECT serialTitle = ChildRect(main, 9001);
    const RECT flow = ChildRect(main, ID_FLOW);
    const RECT receiveTitle = ChildRect(main, 9002);
    const RECT simpleMode = ChildRect(main, ID_SIMPLE_MODE);
    const RECT receiveOnly = ChildRect(main, ID_RX_ONLY);
    const RECT receiveHex = ChildRect(main, ID_RX_HEX);
    const RECT timestamp = ChildRect(main, ID_TIMESTAMP);
    const RECT autoScroll = ChildRect(main, ID_AUTOLINE);
    const RECT sendTitle = ChildRect(main, 9003);
    const RECT timed = ChildRect(main, ID_TIMED);
    const RECT interval = ChildRect(main, ID_INTERVAL);
    const RECT milliseconds = ChildRect(main, 9010);
    const RECT encodingLabel = ChildRect(main, 9011);
    const RECT encoding = ChildRect(main, ID_ENCODING);
    const RECT status = ChildRect(main, ID_STATUS_LEFT);
    const int firstGap = receiveTitle.top - flow.bottom;
    const int secondGap = sendTitle.top - simpleMode.bottom;
    const bool geometryOk = serialTitle.top >= 0 && flow.bottom < receiveTitle.top &&
           receiveTitle.bottom < receiveHex.top && receiveHex.bottom < timestamp.top &&
           timestamp.bottom < autoScroll.top && autoScroll.bottom < receiveOnly.top &&
           receiveOnly.bottom < simpleMode.top && simpleMode.bottom < sendTitle.top &&
           sendTitle.bottom < timed.top && timed.bottom < encodingLabel.top &&
           encodingLabel.bottom <= encoding.top && encoding.bottom < status.top &&
            std::abs(CenterY(timed) - CenterY(interval)) <= 2 &&
           std::abs(CenterY(timed) - CenterY(milliseconds)) <= 2 &&
           secondGap >= 8 && secondGap <= 48 && std::abs(firstGap - secondGap) <= 18;
    return geometryOk;
}

void Click(HWND main, int id) {
    SendMessageW(GetDlgItem(main, id), BM_CLICK, 0, 0);
}

std::wstring WindowText(HWND window) {
    const int length = static_cast<int>(SendMessageW(window, WM_GETTEXTLENGTH, 0, 0));
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(window, WM_GETTEXT, static_cast<WPARAM>(length + 1), reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<std::size_t>(length));
    return text;
}

std::array<LRESULT, 11> MainCardGeometry(HWND main) {
    std::array<LRESULT, 11> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = SendMessageW(main, WM_TEST_QUERY, 20 + index, 0);
    }
    return result;
}

void SetTestValue(HWND main, ULONG_PTR key, const std::wstring& value) {
    COPYDATASTRUCT copy{};
    copy.dwData = key;
    copy.cbData = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    copy.lpData = const_cast<wchar_t*>(value.c_str());
    SendMessageW(main, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy));
}

bool SelectComboText(HWND combo, const std::wstring& value) {
    const LRESULT index = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                       reinterpret_cast<LPARAM>(value.c_str()));
    const LRESULT prefix = index == CB_ERR
        ? SendMessageW(combo, CB_FINDSTRING, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(value.c_str()))
        : index;
    if (prefix == CB_ERR) return false;
    SendMessageW(combo, CB_SETCURSEL, prefix, 0);
    return true;
}

BOOL CALLBACK CollectAppWindows(HWND window, LPARAM parameter) {
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (wcscmp(className, kWindowClass) == 0) reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(window);
    return TRUE;
}

std::vector<HWND> AppWindows() {
    std::vector<HWND> windows;
    EnumWindows(CollectAppWindows, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

struct ChildTextSearch {
    std::wstring text;
    HWND match{};
};

BOOL CALLBACK FindChildText(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<ChildTextSearch*>(parameter);
    if (WindowText(window) == search->text) {
        search->match = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindChildByText(HWND parent, const std::wstring& text) {
    ChildTextSearch search{text};
    EnumChildWindows(parent, FindChildText, reinterpret_cast<LPARAM>(&search));
    return search.match;
}

BOOL CALLBACK FindProcessDialog(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    wchar_t className[64]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (processId == search->processId && wcscmp(className, L"#32770") == 0) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindDialogForProcess(DWORD processId) {
    WindowSearch search{processId, nullptr};
    EnumWindows(FindProcessDialog, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

std::vector<char> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(stream), {});
}

bool IsValidUtf8(const std::vector<char>& bytes) {
    return bytes.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                                 static_cast<int>(bytes.size()), nullptr, 0) > 0;
}

std::wstring ClipboardText() {
    if (!OpenClipboard(nullptr)) return {};
    std::wstring result;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
            result = text;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return result;
}

void RestoreClipboard(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (data) {
        if (auto* target = static_cast<wchar_t*>(GlobalLock(data))) {
            memcpy(target, text.c_str(), bytes);
            GlobalUnlock(data);
            SetClipboardData(CF_UNICODETEXT, data);
        } else GlobalFree(data);
    }
    CloseClipboard();
}

bool CaptureWindowBitmap(HWND window, const std::filesystem::path& path) {
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return false;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!memory || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }
    const auto old = SelectObject(memory, bitmap);
    const BOOL rendered = PrintWindow(window, memory, 2);
    SelectObject(memory, old);
    const DWORD pixelBytes = static_cast<DWORD>(width * height * 4);
    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4d42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + pixelBytes;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (rendered && stream) {
        stream.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
        stream.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
        stream.write(reinterpret_cast<const char*>(pixels), pixelBytes);
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return rendered && stream.good();
}

}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::wcout << std::unitbuf;
    std::wcerr << std::unitbuf;
    if (argc < 2) {
        std::wcerr << L"Usage: UiSmokeTests.exe <SerialMate.exe> [COM port]\n";
        return 2;
    }
    const std::wstring exe = std::filesystem::absolute(argv[1]).wstring();
    const std::wstring port = argc > 2 ? argv[2] : L"COM7";
    const auto outputDirectory = std::filesystem::path(exe).parent_path();
    const auto exportPath = outputDirectory / L"ui-smoke-export.txt";
    const auto rawRxPath = outputDirectory / L"ui-smoke-rx.bin";
    const auto defaultSnapshot1536 = outputDirectory / L"ui-default-1536x1024.bmp";
    const auto defaultSnapshot1920 = outputDirectory / L"ui-default-1920x1080.bmp";
    const auto connectedSnapshot1536 = outputDirectory / L"ui-connected-1536x1024.bmp";
    const auto currentConfig = outputDirectory / L"SerialMate.ini";
    const auto legacyConfig = outputDirectory / L"SerialAssistant.ini";
    const std::wstring originalClipboard = ClipboardText();
    std::error_code ignored;
    std::filesystem::remove(rawRxPath, ignored);
    std::filesystem::remove(currentConfig, ignored);
    std::filesystem::remove(legacyConfig, ignored);
    std::wstring command = L"\"" + exe + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        std::filesystem::path(exe).parent_path().c_str(), &startup, &process)) {
        std::wcerr << L"Unable to start application: " << GetLastError() << L'\n';
        return 3;
    }
    CloseHandle(process.hThread);
    const auto cleanup = [&] {
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 100) == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 9);
            CloseHandle(process.hProcess);
            process.hProcess = nullptr;
        }
    };

    WaitForInputIdle(process.hProcess, 5000);
    HWND main = FindWindowForProcess(process.dwProcessId, std::chrono::seconds(5));
    if (!main) { std::wcerr << L"Main window not found\n"; cleanup(); return 4; }
    RECT client{}; GetClientRect(main, &client);
    const HWND logView = GetDlgItem(main, ID_LOG_VIEW);
    const HWND sendEdit = GetDlgItem(main, ID_SEND_EDIT);
    if (client.right < 900 || client.bottom < 600 || !logView || !sendEdit) {
        std::wcerr << L"Window layout or required controls are missing: client=" << client.right << L"x" << client.bottom
                   << L", log=" << reinterpret_cast<std::uintptr_t>(logView)
                   << L", send=" << reinterpret_cast<std::uintptr_t>(sendEdit) << L'\n';
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 5;
    }
    std::wcout << L"PASS: main window and required controls created\n";
    if (WindowText(GetDlgItem(main, ID_SEND_TIMED)) != L"定时发送" ||
        WindowText(GetDlgItem(main, ID_PAUSE)) != L"暂停显示" ||
        WindowText(GetDlgItem(main, ID_CLEAR_LOG)) != L"清空" ||
        WindowText(GetDlgItem(main, ID_EXPORT)) != L"导出..." ||
        WindowText(GetDlgItem(main, ID_LOAD_FILE)) != L"加载文件...") {
        std::wcerr << L"Test control IDs do not match the application\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 70;
    }
    if (WindowText(main) != L"SerialMate 串口助手") {
        std::wcerr << L"Main window title must not contain a version or transient status\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 55;
    }
    if (!WindowText(sendEdit).empty() ||
        WindowText(GetDlgItem(main, ID_BAUD)) != L"115200" ||
        WindowText(GetDlgItem(main, ID_DATA_BITS)) != L"8" ||
        WindowText(GetDlgItem(main, ID_STOP_BITS)) != L"1" ||
        WindowText(GetDlgItem(main, ID_PARITY)) != L"None" ||
        WindowText(GetDlgItem(main, ID_FLOW)) != L"None" ||
        WindowText(GetDlgItem(main, ID_INTERVAL)) != L"1000" ||
        IsDlgButtonChecked(main, ID_TIMESTAMP) != BST_CHECKED ||
        IsDlgButtonChecked(main, ID_AUTOLINE) != BST_CHECKED ||
        IsDlgButtonChecked(main, ID_SIMPLE_MODE) != BST_UNCHECKED ||
        IsDlgButtonChecked(main, ID_RX_ONLY) != BST_UNCHECKED ||
        IsDlgButtonChecked(main, ID_RX_HEX) != BST_CHECKED ||
        IsDlgButtonChecked(main, ID_TX_HEX) != BST_UNCHECKED ||
        IsDlgButtonChecked(main, ID_TX_CR) != BST_UNCHECKED ||
        IsDlgButtonChecked(main, ID_TX_LF) != BST_UNCHECKED ||
        IsDlgButtonChecked(main, ID_TIMED) != BST_UNCHECKED ||
        SendMessageW(main, WM_TEST_QUERY, 10, 0) != 0) {
        std::wcerr << L"Fresh process did not use the required stateless defaults\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 56;
    }
    if (FindChildByText(main, L"设置") || FindChildByText(main, L"常用指令") ||
        FindChildByText(main, L"AT+VER") || FindChildByText(main, L"AT+RST") ||
        FindChildByText(main, L"AT+HELP") || FindChildByText(main, L"READ?") ||
        FindChildByText(main, L"STATUS?")) {
        std::wcerr << L"Removed settings or preset controls are still present\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 57;
    }
    if (std::filesystem::exists(currentConfig) || std::filesystem::exists(legacyConfig)) {
        std::wcerr << L"Application generated a configuration file during startup\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 58;
    }
    PostMessageW(main, WM_COMMAND, MAKEWPARAM(ID_ABOUT, BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(main, ID_ABOUT)));
    HWND about = nullptr;
    if (!WaitUntil([&] { about = FindDialogForProcess(process.dwProcessId); return about != nullptr; },
                   std::chrono::seconds(2)) ||
        !FindChildByText(about, std::wstring(L"SerialMate 串口助手  v") + version::kCurrent +
            L"\r\n原生 Windows 串口调试工具\r\n作者：如果\r\n支持 UTF-8 / GBK / ASCII、HEX、定时发送、日志与热拔插恢复。")) {
        std::wcerr << L"About dialog did not retain the product version\n";
        if (about) PostMessageW(about, WM_CLOSE, 0, 0);
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 59;
    }
    PostMessageW(about, WM_CLOSE, 0, 0);
    WaitUntil([&] { return !IsWindow(about); }, std::chrono::seconds(2));
    std::wcout << L"PASS: stateless defaults, simplified controls, title and About version\n";
    RECT timestampRect{}, autoScrollRect{};
    GetWindowRect(GetDlgItem(main, ID_TIMESTAMP), &timestampRect);
    GetWindowRect(GetDlgItem(main, ID_AUTOLINE), &autoScrollRect);
    if (autoScrollRect.top <= timestampRect.top) {
        std::wcerr << L"Timestamp and auto-scroll must occupy separate rows\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 37;
    }
    if (WindowText(GetDlgItem(main, ID_TX_CR)) != L"发送新行 (CR)" ||
        WindowText(GetDlgItem(main, ID_TX_LF)) != L"发送新行 (LF)") {
        std::wcerr << L"CR/LF send controls do not use the final labels\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 45;
    }

    SetWindowPos(main, nullptr, 0, 0, 1536, 1024,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateWindow(main);
    const auto initialCardGeometry = MainCardGeometry(main);
    if (initialCardGeometry[0] != initialCardGeometry[6] ||
        initialCardGeometry[5] != initialCardGeometry[9] ||
        initialCardGeometry[2] - initialCardGeometry[1] != initialCardGeometry[10] ||
        initialCardGeometry[4] - initialCardGeometry[3] != initialCardGeometry[10] ||
        initialCardGeometry[8] - initialCardGeometry[7] != initialCardGeometry[10]) {
        std::wcerr << L"Initial card geometry does not share top, bottom and gap baselines\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 63;
    }
    if (SendMessageW(main, WM_TEST_QUERY, 31, 0) != 1) {
        std::wcerr << L"Send editor and record view do not share the canonical HFONT\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 65;
    }
    SetWindowPos(main, nullptr, 0, 0, 1920, 1080,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateWindow(main);
    SetWindowPos(main, nullptr, 0, 0, 1536, 1024,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateWindow(main);
    if (MainCardGeometry(main) != initialCardGeometry) {
        std::wcerr << L"Initial layout differs from resize-back layout\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 64;
    }
    std::wcout << L"PASS: initial and resize-back card geometry are identical\n";

    const std::array<SIZE, 4> geometrySizes{{{1536, 1024}, {1920, 1080},
                                              {1366, 768}, {1280, 720}}};
    for (const auto size : geometrySizes) {
        SetWindowPos(main, nullptr, 0, 0, size.cx, size.cy,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateWindow(main);
        if (!SettingsControlsHaveValidGeometry(main)) {
            std::wcerr << L"Settings geometry failed at " << size.cx << L"x" << size.cy << L'\n';
            PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 54;
        }
        const RECT leftTop = ChildRect(main, 9001);
        const RECT leftBottom = ChildRect(main, ID_ENCODING);
        const RECT rightTop = ChildRect(main, 9004);
        const RECT rightBottom = ChildRect(main, ID_SEND_EDIT);
        if (std::abs(leftTop.top - rightTop.top) > 24 ||
            std::abs(leftBottom.bottom - rightBottom.bottom) > 36) {
            std::wcerr << L"Left and right card stacks are visually unbalanced at "
                       << size.cx << L"x" << size.cy
                       << L" (leftTop=" << leftTop.top << L", rightTop=" << rightTop.top
                       << L", leftBottom=" << leftBottom.bottom << L", rightBottom="
                       << rightBottom.bottom << L", sendEditTop=" << rightBottom.top
                       << L", serial=" << SendMessageW(main, WM_TEST_QUERY, 20, 0)
                       << L"/" << SendMessageW(main, WM_TEST_QUERY, 21, 0)
                       << L", send=" << SendMessageW(main, WM_TEST_QUERY, 24, 0)
                       << L"/" << SendMessageW(main, WM_TEST_QUERY, 25, 0)
                       << L", log=" << SendMessageW(main, WM_TEST_QUERY, 26, 0)
                       << L"/" << SendMessageW(main, WM_TEST_QUERY, 27, 0)
                       << L", data=" << SendMessageW(main, WM_TEST_QUERY, 28, 0)
                       << L"/" << SendMessageW(main, WM_TEST_QUERY, 29, 0) << L")\n";
            PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 60;
        }
    }
    SetWindowPos(main, nullptr, 0, 0, 1536, 1024,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateWindow(main);
    if (!CaptureWindowBitmap(main, defaultSnapshot1536)) {
        std::wcerr << L"Unable to capture 1536x1024 default UI snapshot\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 61;
    }
    SetWindowPos(main, nullptr, 0, 0, 1920, 1080,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateWindow(main);
    if (!CaptureWindowBitmap(main, defaultSnapshot1920)) {
        std::wcerr << L"Unable to capture 1920x1080 default UI snapshot\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 62;
    }
    SetWindowPos(main, nullptr, 0, 0, 1536, 1024,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateWindow(main);
    std::wcout << L"PASS: balanced card geometry and default snapshots at 1536/1920\n";

    if (!SelectComboText(GetDlgItem(main, ID_PORT), port)) {
        std::wcerr << port << L" is not present in the port selector\n"; PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 6;
    }
    Click(main, ID_LOG);
    Click(main, ID_OPEN);
    if (!WaitUntil([&] { return !IsWindowEnabled(GetDlgItem(main, ID_OPEN)) && IsWindowEnabled(GetDlgItem(main, ID_CLOSE)); }, std::chrono::seconds(3))) {
        std::wcerr << L"UI did not enter connected state\n"; PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 7;
    }
    std::wcout << L"PASS: UI opened " << port << L"\n";

    // Exercise a close/open cycle after an injected stale caption. The product
    // title must always return to its fixed, version-free value.
    Click(main, ID_CLOSE);
    if (!SetWindowTextW(main, L"SerialMate 串口助手    设备已断开，可重新扫描并连接")) {
        std::wcerr << L"Unable to prepare reconnect caption regression\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 31;
    }
    Click(main, ID_OPEN);
    wchar_t reconnectTitle[128]{};
    GetWindowTextW(main, reconnectTitle, static_cast<int>(std::size(reconnectTitle)));
    if (SendMessageW(main, WM_TEST_QUERY, 0, 0) != 1 ||
        std::wstring(reconnectTitle) != L"SerialMate 串口助手") {
        std::wcerr << L"Reconnect did not clear the stale disconnected caption\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 32;
    }
    std::wcout << L"PASS: reconnect clears stale disconnected caption\n";
    SendMessageW(GetDlgItem(main, ID_TX_HEX), BM_SETCHECK, BST_UNCHECKED, 0);

    SetTestValue(main, 1, L"UI-SMOKE");
    SendMessageW(GetDlgItem(main, ID_TX_CR), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(GetDlgItem(main, ID_TX_LF), BM_SETCHECK, BST_CHECKED, 0);
    Click(main, ID_SEND);
    if (!WaitUntil([&] {
        return SendMessageW(main, WM_TEST_QUERY, 1, 0) >= 10 &&
               SendMessageW(main, WM_TEST_QUERY, 2, 0) >= 10 &&
               SendMessageW(main, WM_TEST_QUERY, 3, 0) >= 2;
    }, std::chrono::seconds(4))) {
        std::wcerr << L"UI loopback counters/log did not advance: RX=" << SendMessageW(main, WM_TEST_QUERY, 1, 0)
                   << L", TX=" << SendMessageW(main, WM_TEST_QUERY, 2, 0)
                   << L", log=" << SendMessageW(main, WM_TEST_QUERY, 3, 0) << L'\n';
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 8;
    }
    std::wcout << L"PASS: UI ASCII send and RX/TX communication record\n";
    SetTestValue(main, 1, L"ENTER");
    const LRESULT txBeforeEnter = SendMessageW(main, WM_TEST_QUERY, 2, 0);
    SendMessageW(sendEdit, WM_KEYDOWN, VK_RETURN, 0);
    if (!WaitUntil([&] { return SendMessageW(main, WM_TEST_QUERY, 2, 0) >= txBeforeEnter + 7; },
                   std::chrono::seconds(3))) {
        std::wcerr << L"Enter key did not send the editor contents\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 68;
    }
    Click(main, ID_CLEAR_SEND);
    if (!WindowText(sendEdit).empty()) {
        std::wcerr << L"Clear did not empty the send editor\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 69;
    }
    std::wcout << L"PASS: Enter send and send-editor clear\n";
    SetTestValue(main, 1, L"中文");
    const LRESULT rxBeforeChinese = SendMessageW(main, WM_TEST_QUERY, 1, 0);
    Click(main, ID_SEND);
    if (!WaitUntil([&] {
        return SendMessageW(main, WM_TEST_QUERY, 1, 0) >= rxBeforeChinese + 8;
    }, std::chrono::seconds(3))) {
        std::wcerr << L"UI did not receive UTF-8 Chinese loopback\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 49;
    }
    SetTestValue(main, 8, L"");
    if (!WaitUntil([&] { return ClipboardText().find(L"中文") != std::wstring::npos; },
                   std::chrono::seconds(2))) {
        std::wcerr << L"Communication TEXT copy did not preserve UTF-8 Chinese\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 50;
    }
    std::wcout << L"PASS: UTF-8 Chinese TEXT display and copy\n";
    if (SendMessageW(main, WM_TEST_QUERY, 6, 0) <= 0 || SendMessageW(main, WM_TEST_QUERY, 7, 0) <= 0) {
        std::wcerr << L"Communication view metrics were not available\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 35;
    }
    const LRESULT hexWithTimestamp = SendMessageW(main, WM_TEST_QUERY, 8, 0);
    SendMessageW(GetDlgItem(main, ID_TIMESTAMP), BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(main, WM_COMMAND, MAKEWPARAM(ID_TIMESTAMP, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(main, ID_TIMESTAMP)));
    if (SendMessageW(main, WM_TEST_QUERY, 8, 0) >= hexWithTimestamp) {
        std::wcerr << L"Hiding timestamps did not move communication columns left\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 38;
    }
    SendMessageW(GetDlgItem(main, ID_TIMESTAMP), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(main, WM_COMMAND, MAKEWPARAM(ID_TIMESTAMP, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(main, ID_TIMESTAMP)));
    RECT recordClient{};
    GetClientRect(logView, &recordClient);
    const LRESULT defaultBytesPerRow = SendMessageW(main, WM_TEST_QUERY, 9, 0);
    if ((defaultBytesPerRow != 8 && defaultBytesPerRow != 12 && defaultBytesPerRow != 16) ||
        SendMessageW(main, WM_TEST_QUERY, 7, 0) > recordClient.right) {
        std::wcerr << L"Default communication layout hides TEXT or requires horizontal scrolling\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 46;
    }
    SetWindowPos(main, nullptr, 0, 0, 1340, 780, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetClientRect(logView, &recordClient);
    if (SendMessageW(main, WM_TEST_QUERY, 9, 0) < 8 ||
        SendMessageW(main, WM_TEST_QUERY, 7, 0) > recordClient.right) {
        std::wcerr << L"Minimum window size does not keep 8-byte HEX/TEXT visible\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 48;
    }
    SetWindowPos(main, nullptr, 0, 0, 1536, 1024, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    std::wcout << L"PASS: timestamp column hides and layout reflows\n";

    SetTestValue(main, 1, L"C");
    SendMessageW(GetDlgItem(main, ID_TX_CR), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(GetDlgItem(main, ID_TX_LF), BM_SETCHECK, BST_UNCHECKED, 0);
    LRESULT rxBeforeEnding = SendMessageW(main, WM_TEST_QUERY, 1, 0);
    Click(main, ID_SEND);
    if (!WaitUntil([&] { return SendMessageW(main, WM_TEST_QUERY, 1, 0) >= rxBeforeEnding + 2; }, std::chrono::seconds(3))) {
        std::wcerr << L"仅 CR 发送失败\n"; PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 24;
    }
    SetTestValue(main, 1, L"L");
    SendMessageW(GetDlgItem(main, ID_TX_CR), BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(main, ID_TX_LF), BM_SETCHECK, BST_CHECKED, 0);
    rxBeforeEnding = SendMessageW(main, WM_TEST_QUERY, 1, 0);
    Click(main, ID_SEND);
    if (!WaitUntil([&] { return SendMessageW(main, WM_TEST_QUERY, 1, 0) >= rxBeforeEnding + 2; }, std::chrono::seconds(3))) {
        std::wcerr << L"LF-only send failed\n"; PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 25;
    }
    SendMessageW(GetDlgItem(main, ID_TX_CR), BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(main, ID_TX_LF), BM_SETCHECK, BST_UNCHECKED, 0);
    SetTestValue(main, 1, L"N");
    rxBeforeEnding = SendMessageW(main, WM_TEST_QUERY, 1, 0);
    Click(main, ID_SEND);
    if (!WaitUntil([&] { return SendMessageW(main, WM_TEST_QUERY, 1, 0) >= rxBeforeEnding + 1; }, std::chrono::seconds(3))) {
        std::wcerr << L"No-suffix send failed\n"; PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 43;
    }
    SendMessageW(GetDlgItem(main, ID_TX_CR), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(GetDlgItem(main, ID_TX_LF), BM_SETCHECK, BST_CHECKED, 0);
    std::wcout << L"PASS: CR-only, LF-only, CRLF and no-suffix send\n";

    SetTestValue(main, 1, L"AT+VER");
    SendMessageW(GetDlgItem(main, ID_TX_HEX), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(main, WM_COMMAND, MAKEWPARAM(ID_TX_HEX, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(main, ID_TX_HEX)));
    const std::wstring convertedHex = WindowText(GetDlgItem(main, ID_SEND_EDIT));
    if (convertedHex != L"41 54 2B 56 45 52") {
        std::wcerr << L"Strict HEX mode did not convert text to normalized bytes: " << convertedHex << L'\n';
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 39;
    }
    SetTestValue(main, 1, L"ggffz 0a");
    SendMessageW(main, WM_COMMAND, MAKEWPARAM(ID_SEND_EDIT, EN_CHANGE), reinterpret_cast<LPARAM>(GetDlgItem(main, ID_SEND_EDIT)));
    if (WindowText(GetDlgItem(main, ID_SEND_EDIT)) != L"FF 0A") {
        std::wcerr << L"Strict HEX editor retained illegal characters\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 44;
    }
    SetTestValue(main, 1, L"00 01 7f 80 fe ff");
    SendMessageW(main, WM_COMMAND, MAKEWPARAM(ID_SEND_EDIT, EN_CHANGE), reinterpret_cast<LPARAM>(GetDlgItem(main, ID_SEND_EDIT)));
    const std::wstring normalizedHex = WindowText(GetDlgItem(main, ID_SEND_EDIT));
    if (normalizedHex != L"00 01 7F 80 FE FF") {
        std::wcerr << L"Strict HEX editor did not normalize case and spacing: " << normalizedHex << L'\n';
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 40;
    }
    const LRESULT rxBeforeHex = SendMessageW(main, WM_TEST_QUERY, 1, 0);
    const LRESULT txBeforeHex = SendMessageW(main, WM_TEST_QUERY, 2, 0);
    SetTestValue(main, 14, rawRxPath.wstring());
    if (SendMessageW(main, WM_TEST_QUERY, 11, 0) != 1) {
        std::wcerr << L"Raw RX writer did not start\n"; PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 51;
    }
    Click(main, ID_SEND);
    if (!WaitUntil([&] {
        return SendMessageW(main, WM_TEST_QUERY, 1, 0) >= rxBeforeHex + 8 &&
               SendMessageW(main, WM_TEST_QUERY, 2, 0) >= txBeforeHex + 8;
    }, std::chrono::seconds(4))) {
        std::wcerr << L"UI HEX send record missing\n"; PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 9;
    }
    SetTestValue(main, 14, L"");
    const auto rawBytes = ReadFileBytes(rawRxPath);
    const std::array<char, 8> expectedRaw{0x00, 0x01, 0x7f, static_cast<char>(0x80),
                                          static_cast<char>(0xfe), static_cast<char>(0xff), 0x0d, 0x0a};
    if (SendMessageW(main, WM_TEST_QUERY, 11, 0) != 0 ||
        rawBytes != std::vector<char>(expectedRaw.begin(), expectedRaw.end())) {
        std::wcerr << L"Raw RX file did not preserve binary loopback bytes exactly\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 53;
    }
    std::wcout << L"PASS: raw RX file preserves bytes independently from structured log\n";
    std::wcout << L"PASS: UI HEX send\n";
    if (SendMessageW(main, WM_TEST_QUERY, 7, 0) <= 0) {
        std::wcerr << L"Communication view metrics were not available\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 36;
    }

    // File loading populates the editor; it is not the removed raw-file sender.
    // HEX mode therefore takes a file containing HEX text, not arbitrary binary.
    const auto checkFileLoopback = [&](const wchar_t* name, const std::string& fileBytes,
                                       const std::wstring& editorText, const std::vector<char>& expected,
                                       int encodingIndex, bool hex) {
        const auto inputPath = outputDirectory / (std::wstring(name) + L"-input.txt");
        const auto capturePath = outputDirectory / (std::wstring(name) + L"-rx.bin");
        {
            std::ofstream input(inputPath, std::ios::binary | std::ios::trunc);
            input.write(fileBytes.data(), static_cast<std::streamsize>(fileBytes.size()));
            input.close();
            if (!input) { std::wcerr << name << L": cannot write fixture\n"; return false; }
            // RawRxWriter appends, so each run needs a fresh test capture.
            std::ofstream capture(capturePath, std::ios::binary | std::ios::trunc);
            capture.close();
            if (!capture) { std::wcerr << name << L": cannot reset test capture\n"; return false; }
        }
        const auto setCheck = [&](int id, bool checked) {
            SendMessageW(GetDlgItem(main, id), BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(main, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(main, id)));
        };
        setCheck(ID_TIMED, false);
        SetTestValue(main, 1, L"");
        setCheck(ID_TX_HEX, hex);
        setCheck(ID_TX_CR, false);
        setCheck(ID_TX_LF, false);
        SendMessageW(GetDlgItem(main, ID_ENCODING), CB_SETCURSEL, encodingIndex, 0);
        SendMessageW(main, WM_COMMAND, MAKEWPARAM(ID_ENCODING, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(GetDlgItem(main, ID_ENCODING)));
        SetTestValue(main, 3, inputPath.wstring());
        if (SendMessageW(main, WM_TEST_QUERY, 10, 0) != encodingIndex || WindowText(sendEdit) != editorText) {
            std::wcerr << name << L": loaded editor text or encoding mismatch\n";
            return false;
        }
        const LRESULT rxBefore = SendMessageW(main, WM_TEST_QUERY, 1, 0);
        const LRESULT txBefore = SendMessageW(main, WM_TEST_QUERY, 2, 0);
        if (rxBefore != txBefore) {
            std::wcerr << name << L": previous loopback has not drained\n"; return false;
        }
        SetTestValue(main, 14, capturePath.wstring());
        if (SendMessageW(main, WM_TEST_QUERY, 11, 0) != 1) {
            std::wcerr << name << L": capture did not start\n"; return false;
        }
        Click(main, ID_SEND);
        const auto count = static_cast<LRESULT>(expected.size());
        const bool received = WaitUntil([&] {
            return SendMessageW(main, WM_TEST_QUERY, 1, 0) >= rxBefore + count &&
                   SendMessageW(main, WM_TEST_QUERY, 2, 0) >= txBefore + count;
        }, std::chrono::seconds(5));
        SetTestValue(main, 14, L"");
        const auto actual = ReadFileBytes(capturePath);
        const auto rx = SendMessageW(main, WM_TEST_QUERY, 1, 0) - rxBefore;
        const auto tx = SendMessageW(main, WM_TEST_QUERY, 2, 0) - txBefore;
        if (!received || rx != count || tx != count || actual != expected ||
            SendMessageW(main, WM_TEST_QUERY, 11, 0) != 0) {
            const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected.begin(), expected.end());
            std::wcerr << name << L": expected=" << count << L", RX=" << rx << L", TX=" << tx
                       << L", captured=" << actual.size() << L", first difference="
                       << std::distance(actual.begin(), mismatch.first) << L'\n';
            return false;
        }
        std::wcout << L"PASS: file/editor/send serial loopback " << name << L" (" << count << L" bytes, exact match)\n";
        return true;
    };
    std::string ascii;
    for (int i = 0; i < 256; ++i) ascii += "0123456789ABCDEF";
    const std::string chinese = u8"你在干嘛";
    const std::string gbk = "\xC4\xE3\xD4\xDA\xB8\xC9\xC2\xEF";
    if (!checkFileLoopback(L"ascii-4096", ascii, std::wstring(ascii.begin(), ascii.end()),
                           std::vector<char>(ascii.begin(), ascii.end()), 2, false) ||
        !checkFileLoopback(L"utf8-chinese", chinese, L"你在干嘛",
                           std::vector<char>(chinese.begin(), chinese.end()), 0, false) ||
        !checkFileLoopback(L"gbk-chinese", gbk, L"你在干嘛",
                           std::vector<char>(gbk.begin(), gbk.end()), 1, false) ||
        !checkFileLoopback(L"hex-text", "00 01 7F 80 FE FF 0D 0A", L"00 01 7F 80 FE FF 0D 0A",
                           std::vector<char>(expectedRaw.begin(), expectedRaw.end()), 0, true)) {
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 16;
    }

    SendMessageW(GetDlgItem(main, ID_TX_HEX), BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(main, WM_COMMAND, MAKEWPARAM(ID_TX_HEX, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(main, ID_TX_HEX)));
    SetTestValue(main, 1, L"TIMER");
    SetTestValue(main, 2, L"50");
    const LRESULT beforeTimer = SendMessageW(main, WM_TEST_QUERY, 2, 0);
    SendMessageW(GetDlgItem(main, ID_TIMED), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(main, WM_COMMAND, MAKEWPARAM(ID_TIMED, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(main, ID_TIMED)));
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    SendMessageW(GetDlgItem(main, ID_TIMED), BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(main, WM_COMMAND, MAKEWPARAM(ID_TIMED, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(main, ID_TIMED)));
    if (SendMessageW(main, WM_TEST_QUERY, 2, 0) <= beforeTimer + 10) {
        std::wcerr << L"Timed send produced no visible records\n"; PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 10;
    }
    std::wcout << L"PASS: timed send\n";

    const LRESULT rxBeforePause = SendMessageW(main, WM_TEST_QUERY, 1, 0);
    Click(main, ID_PAUSE);
    if (SendMessageW(main, WM_TEST_QUERY, 4, 0) != 1 || WindowText(GetDlgItem(main, ID_PAUSE)) != L"继续显示") {
        std::wcerr << L"Pause state or button label is incorrect\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 41;
    }
    SetTestValue(main, 1, L"PAUSED-RX");
    Click(main, ID_SEND);
    if (!WaitUntil([&] { return SendMessageW(main, WM_TEST_QUERY, 1, 0) > rxBeforePause; }, std::chrono::seconds(3))) {
        std::wcerr << L"RX/TX counters did not advance while display was paused\n"; PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 11;
    }
    Click(main, ID_PAUSE);
    if (SendMessageW(main, WM_TEST_QUERY, 4, 0) != 0 || WindowText(GetDlgItem(main, ID_PAUSE)) != L"暂停显示") {
        std::wcerr << L"Resume state or button label is incorrect\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 42;
    }
    std::wcout << L"PASS: background receive continues while display is paused\n";

    SetTestValue(main, 4, exportPath.wstring());
    if (!std::filesystem::exists(exportPath) || std::filesystem::file_size(exportPath) < 100) {
        std::wcerr << L"Communication record export failed\n"; PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 17;
    }
    const auto systemExportBytes = ReadFileBytes(exportPath);
    const std::string systemExportText(systemExportBytes.begin(), systemExportBytes.end());
    if (!IsValidUtf8(systemExportBytes) || systemExportText.find("UI-SMOKE") == std::string::npos ||
        systemExportText.find(u8"已连接 ") != std::string::npos ||
        systemExportText.find(u8"串口已关闭") != std::string::npos ||
        systemExportText.find("NOTICE") != std::string::npos || systemExportText.find("ERROR") != std::string::npos) {
        std::wcerr << L"Export is invalid UTF-8, missing traffic, or includes hidden system messages\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 47;
    }
    std::wcout << L"PASS: communication record export\n";
    Click(main, ID_CLEAR_LOG);
    SendMessageW(GetDlgItem(main, ID_TX_HEX), BM_SETCHECK, BST_UNCHECKED, 0);
    const std::array<const wchar_t*, 4> snapshotCommands{L"AT", L"AT+VER", L"中文", L"READ?"};
    for (const wchar_t* commandText : snapshotCommands) {
        SetTestValue(main, 1, commandText);
        Click(main, ID_SEND);
        std::this_thread::sleep_for(std::chrono::milliseconds(90));
    }
    if (!CaptureWindowBitmap(main, connectedSnapshot1536)) {
        std::wcerr << L"Unable to capture UI snapshot\n"; PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 23;
    }
    std::wcout << L"PASS: UI snapshot captured\n";

    SetTestValue(main, 8, L"");
    if (!WaitUntil([&] { const auto text = ClipboardText(); return text.find(L"│ ") != std::wstring::npos; }, std::chrono::seconds(2))) {
        std::wcerr << L"Copy-all did not place communication text on clipboard\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); RestoreClipboard(originalClipboard); return 27;
    }
    const std::wstring copiedAll = ClipboardText();
    SetTestValue(main, 7, L"x");
    if (!WaitUntil([&] { const auto text = ClipboardText(); return !text.empty() && text != copiedAll; }, std::chrono::seconds(2))) {
        std::wcerr << L"Selected copy did not place text on clipboard\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); RestoreClipboard(originalClipboard); return 28;
    }
    const std::wstring copiedSelection = ClipboardText();
    if (copiedSelection.find(L"[") == std::wstring::npos) {
        std::wcerr << L"Selected copy did not preserve the complete record\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); RestoreClipboard(originalClipboard); return 30;
    }
    std::wcout << L"PASS: copy menu modes and selected copy\n";

    const std::wstring stressChunk(1024 * 1024 + 256 * 1024, L'X');
    SetTestValue(main, 6, stressChunk);
    SetTestValue(main, 6, L"trim-check");
    const LRESULT boundedLogLength = SendMessageW(main, WM_TEST_QUERY, 3, 0);
    if (boundedLogLength < 65536 || boundedLogLength > 262144) {
        std::wcerr << L"Communication raw row bound failed: " << boundedLogLength << L" rows\n";
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 26;
    }
    std::wcout << L"PASS: communication view remains bounded after 1.25 MiB append stress\n";

    Click(main, ID_LOG);
    Click(main, ID_CLOSE);
    if (!WaitUntil([&] { return IsWindowEnabled(GetDlgItem(main, ID_OPEN)) && !IsWindowEnabled(GetDlgItem(main, ID_CLOSE)); }, std::chrono::seconds(3))) {
        std::wcerr << L"UI did not close the serial port\n"; PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 12;
    }
    std::wcout << L"PASS: UI closed serial port\n";

    const auto beforeNewWindow = AppWindows();
    Click(main, ID_NEW);
    HWND newWindow = nullptr;
    if (!WaitUntil([&] {
        const auto windows = AppWindows();
        for (HWND candidate : windows) {
            if (std::find(beforeNewWindow.begin(), beforeNewWindow.end(), candidate) == beforeNewWindow.end()) {
                newWindow = candidate;
                return true;
            }
        }
        return false;
    }, std::chrono::seconds(5))) {
        std::wcerr << L"New window was not created\n"; PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 18;
    }
    if (!WindowText(GetDlgItem(newWindow, ID_SEND_EDIT)).empty()) {
        std::wcerr << L"New window did not start with an empty send editor\n";
        PostMessageW(newWindow, WM_CLOSE, 0, 0);
        PostMessageW(main, WM_CLOSE, 0, 0); cleanup(); return 63;
    }
    PostMessageW(newWindow, WM_CLOSE, 0, 0);
    WaitUntil([&] { return !IsWindow(newWindow); }, std::chrono::seconds(3));
    std::wcout << L"PASS: new window / multi-instance\n";

    PostMessageW(main, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(process.hProcess, 4000) != WAIT_OBJECT_0) {
        std::wcerr << L"Application did not exit promptly\n"; cleanup(); return 13;
    }
    DWORD applicationExitCode = 0;
    const BOOL exitCodeResult = GetExitCodeProcess(process.hProcess, &applicationExitCode);
    cleanup();
    if (!exitCodeResult || applicationExitCode != 0) return 14;
    if (std::filesystem::exists(currentConfig) || std::filesystem::exists(legacyConfig)) {
        std::wcerr << L"Application generated a configuration file during use or shutdown\n";
        return 15;
    }
    const auto exportBytes = ReadFileBytes(exportPath);
    if (exportBytes.size() < 3 || static_cast<unsigned char>(exportBytes[0]) != 0xef ||
        static_cast<unsigned char>(exportBytes[1]) != 0xbb || static_cast<unsigned char>(exportBytes[2]) != 0xbf ||
        !IsValidUtf8(exportBytes)) {
        std::wcerr << L"Exported communication record is not UTF-8 with BOM\n"; return 21;
    }
    bool foundLog = false;
    for (const auto& entry : std::filesystem::directory_iterator(outputDirectory)) {
        if (entry.path().extension() != L".log" || entry.path().filename().wstring().rfind(L"SerialMate-", 0) != 0) continue;
        const auto logBytes = ReadFileBytes(entry.path());
        const std::string logText(logBytes.begin(), logBytes.end());
        if (IsValidUtf8(logBytes) && logText.find("UI-SMOKE") != std::string::npos && logText.find("TIMER") != std::string::npos) {
            foundLog = true;
            break;
        }
    }
    if (!foundLog) { std::wcerr << L"UTF-8 communication log with expected records was not found\n"; return 22; }
    STARTUPINFOW restartStartup{sizeof(restartStartup)};
    PROCESS_INFORMATION restartProcess{};
    std::wstring restartCommand = L"\"" + exe + L"\"";
    if (!CreateProcessW(exe.c_str(), restartCommand.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        outputDirectory.c_str(), &restartStartup, &restartProcess)) {
        std::wcerr << L"Unable to restart application for stateless-default verification\n";
        return 64;
    }
    CloseHandle(restartProcess.hThread);
    WaitForInputIdle(restartProcess.hProcess, 5000);
    HWND restarted = FindWindowForProcess(restartProcess.dwProcessId, std::chrono::seconds(5));
    if (!restarted || WindowText(restarted) != L"SerialMate 串口助手" ||
        !WindowText(GetDlgItem(restarted, ID_SEND_EDIT)).empty() ||
        WindowText(GetDlgItem(restarted, ID_BAUD)) != L"115200" ||
        WindowText(GetDlgItem(restarted, ID_INTERVAL)) != L"1000" ||
        IsDlgButtonChecked(restarted, ID_TIMESTAMP) != BST_CHECKED ||
        IsDlgButtonChecked(restarted, ID_AUTOLINE) != BST_CHECKED ||
        IsDlgButtonChecked(restarted, ID_TX_HEX) != BST_UNCHECKED ||
        IsDlgButtonChecked(restarted, ID_TX_CR) != BST_UNCHECKED ||
        IsDlgButtonChecked(restarted, ID_TX_LF) != BST_UNCHECKED ||
        IsDlgButtonChecked(restarted, ID_TIMED) != BST_UNCHECKED ||
        SendMessageW(restarted, WM_TEST_QUERY, 10, 0) != 0) {
        std::wcerr << L"Restart did not restore the required stateless defaults\n";
        if (restarted) PostMessageW(restarted, WM_CLOSE, 0, 0);
        WaitForSingleObject(restartProcess.hProcess, 2000);
        CloseHandle(restartProcess.hProcess);
        return 65;
    }
    PostMessageW(restarted, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(restartProcess.hProcess, 4000) != WAIT_OBJECT_0) {
        TerminateProcess(restartProcess.hProcess, 9);
        CloseHandle(restartProcess.hProcess);
        std::wcerr << L"Restarted application did not exit promptly\n";
        return 66;
    }
    CloseHandle(restartProcess.hProcess);
    if (std::filesystem::exists(currentConfig) || std::filesystem::exists(legacyConfig)) {
        std::wcerr << L"Restart generated a configuration file\n";
        return 67;
    }
    RestoreClipboard(originalClipboard);
    std::wcout << L"PASS: graceful exit, no configuration files, stateless restart defaults\n";
    std::wcout << L"PASS: UTF-8 logs and exported communication record\n";
    std::wcout << L"All UI smoke tests passed.\n";
    return 0;
}
