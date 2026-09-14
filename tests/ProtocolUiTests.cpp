#include "ExtensionControlIds.h"
#include "Product.h"
#include "UiGeometry.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr UINT kQuery = WM_APP + 100;
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
    const int length = static_cast<int>(Message(window, WM_GETTEXTLENGTH));
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    text.resize(static_cast<std::size_t>(Message(window, WM_GETTEXT, text.size(),
                                                 reinterpret_cast<LPARAM>(text.data()))));
    return text;
}

RECT ChildRect(HWND parent, HWND child) {
    RECT rect{};
    Check(child != nullptr && GetWindowRect(child, &rect) != FALSE, "child rectangle unavailable");
    MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

bool Contains(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
           inner.bottom <= outer.bottom;
}

struct TestProcess {
    PROCESS_INFORMATION process{};
    HWND window = nullptr;
    std::filesystem::path directory;
    ~TestProcess() {
        if (IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 3000) != WAIT_OBJECT_0)
                TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
        std::error_code ignored;
        if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
    }
};

BOOL CALLBACK FindWindowForProcess(HWND candidate, LPARAM parameter) {
    auto& app = *reinterpret_cast<TestProcess*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(candidate, &processId);
    wchar_t className[128]{};
    GetClassNameW(candidate, className, static_cast<int>(std::size(className)));
    if (processId == app.process.dwProcessId && wcscmp(className, product::kMainWindowClass) == 0) {
        app.window = candidate;
        return FALSE;
    }
    return TRUE;
}

void Start(TestProcess& app, const std::filesystem::path& source) {
    app.directory = std::filesystem::temp_directory_path() /
        (L"SerialMate-ProtocolUi-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(app.directory, ignored);
    std::filesystem::create_directories(app.directory);
    const auto executable = app.directory / L"SerialMate.exe";
    Check(CopyFileW(std::filesystem::absolute(source).c_str(), executable.c_str(), FALSE) != FALSE,
          "cannot copy SerialMate for isolated test");
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNOACTIVATE;
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                         app.directory.c_str(), &startup, &app.process) != FALSE,
          "cannot start isolated SerialMate");
    WaitForInputIdle(app.process.hProcess, 5000);
    for (int attempt = 0; attempt < 100 && !app.window; ++attempt) {
        EnumWindows(FindWindowForProcess, reinterpret_cast<LPARAM>(&app));
        if (!app.window) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Check(app.window != nullptr, "SerialMate window was not created");
}

void ResizeClient(HWND window, int width, int height) {
    RECT outer{}, client{};
    Check(GetWindowRect(window, &outer) != FALSE && GetClientRect(window, &client) != FALSE,
          "window rectangles unavailable");
    Check(SetWindowPos(window, nullptr, outer.left, outer.top,
                       width + (outer.right - outer.left) - client.right,
                       height + (outer.bottom - outer.top) - client.bottom,
                       SWP_NOACTIVATE | SWP_NOZORDER) != FALSE,
          "cannot resize protocol UI test window");
    Message(window, WM_NULL);
}

void SelectFunction(HWND window, int index) {
    HWND combo = GetDlgItem(window, protocolui::FunctionCombo);
    Check(Message(combo, CB_SETCURSEL, index) == index, "cannot select protocol function");
    Message(window, WM_COMMAND, MAKEWPARAM(protocolui::FunctionCombo, CBN_SELCHANGE),
            reinterpret_cast<LPARAM>(combo));
}

void CheckVisible(HWND window, int id, bool expected, const char* message) {
    const HWND control = GetDlgItem(window, id);
    Check(control != nullptr, "protocol control was not pre-created");
    Check((IsWindowVisible(control) != FALSE) == expected, message);
}

void CheckForm(HWND window) {
    const std::vector<int> ids{
        protocolui::TypeLabel, protocolui::TypeCombo, protocolui::SlaveLabel,
        protocolui::SlaveEdit, protocolui::FunctionLabel, protocolui::FunctionCombo,
        protocolui::AddressLabel, protocolui::AddressEdit, protocolui::QuantityLabel,
        protocolui::QuantityEdit, protocolui::DataLabel, protocolui::DataEdit,
        protocolui::Generate, protocolui::FillCustom, protocolui::ResultLabel,
        protocolui::ResultEdit, protocolui::Copy, protocolui::Status};
    for (int id : ids) CheckVisible(window, id, false, "protocol control visible in Standard Mode");

    RECT client{};
    GetClientRect(window, &client);
    ResizeClient(window, static_cast<int>(Message(window, kQuery, 42)), client.bottom);
    GetClientRect(window, &client);
    const auto geometry = CalculateMainLayoutGeometry(client.right, client.bottom,
                                                       GetDpiForWindow(window),
                                                       static_cast<int>(Message(window, kQuery, 51)));
    Check(geometry.extensionVisible, "full extension did not become visible");
    for (int id : ids) {
        if (id == protocolui::DataLabel || id == protocolui::DataEdit) continue;
        CheckVisible(window, id, true, "protocol control hidden in full extension mode");
        const RECT actual = ChildRect(window, GetDlgItem(window, id));
        if (!Contains(geometry.protocolCard, actual))
            std::cerr << "OutsideId=" << id << " Card=[" << geometry.protocolCard.left << ','
                      << geometry.protocolCard.top << ',' << geometry.protocolCard.right << ','
                      << geometry.protocolCard.bottom << "] Actual=[" << actual.left << ','
                      << actual.top << ',' << actual.right << ',' << actual.bottom << "]\n";
        Check(Contains(geometry.protocolCard, actual), "protocol control lies outside protocol Card");
    }

    HWND type = GetDlgItem(window, protocolui::TypeCombo);
    Check(Message(type, CB_GETCOUNT) == 1 && Text(type) == L"Modbus RTU",
          "protocol combo is not fixed to Modbus RTU");
    Check(Text(GetDlgItem(window, protocolui::SlaveEdit)) == L"01",
          "default slave address is not 01");

    for (int index : {0, 1}) {
        SelectFunction(window, index);
        Check(Text(GetDlgItem(window, protocolui::AddressLabel)) == L"起始地址" &&
                  Text(GetDlgItem(window, protocolui::QuantityLabel)) == L"寄存器数量",
              "03/04 parameter labels are incorrect");
        CheckVisible(window, protocolui::DataEdit, false, "03/04 write data is visible");
    }

    SelectFunction(window, 2);
    Check(Text(GetDlgItem(window, protocolui::AddressLabel)) == L"寄存器地址" &&
              Text(GetDlgItem(window, protocolui::QuantityLabel)) == L"写入值",
          "06 parameter labels are incorrect");
    CheckVisible(window, protocolui::DataEdit, false, "06 write data is visible");

    SelectFunction(window, 3);
    Check(Text(GetDlgItem(window, protocolui::AddressLabel)) == L"起始地址" &&
              Text(GetDlgItem(window, protocolui::QuantityLabel)) == L"寄存器数量",
          "10 parameter labels are incorrect");
    CheckVisible(window, protocolui::DataLabel, true, "10 write data label is hidden");
    CheckVisible(window, protocolui::DataEdit, true, "10 write data edit is hidden");
    Check(Contains(geometry.protocolCard,
                   ChildRect(window, GetDlgItem(window, protocolui::DataEdit))),
          "10 write data edit lies outside protocol Card");
    for (int id : ids) {
        if (!IsWindowVisible(GetDlgItem(window, id))) continue;
        Check(Contains(geometry.protocolCard, ChildRect(window, GetDlgItem(window, id))),
              "10 form control lies outside protocol Card");
    }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    TestProcess app;
    try {
        Start(app, argv[1]);
        CheckForm(app.window);
        std::cout << "Protocol UI tests passed. Checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << " (check " << checks << ")\n";
        return 1;
    }
}
