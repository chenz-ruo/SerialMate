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
constexpr int kTxHexId = 117;
constexpr int kSendEditId = 128;
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

void SetText(HWND window, int id, const wchar_t* text) {
    Check(Message(GetDlgItem(window, id), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text)) != FALSE,
          "cannot set protocol test text");
}

void Click(HWND window, int id) {
    Message(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
            reinterpret_cast<LPARAM>(GetDlgItem(window, id)));
}

void PutClipboard(const wchar_t* text) {
    const std::size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    Check(memory != nullptr, "cannot allocate clipboard seed");
    void* target = GlobalLock(memory);
    Check(target != nullptr, "cannot lock clipboard seed");
    memcpy(target, text, bytes);
    GlobalUnlock(memory);
    HWND owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(owner != nullptr, "cannot create clipboard owner window");
    Check(OpenClipboard(owner) != FALSE, "cannot open clipboard for seed");
    EmptyClipboard();
    Check(SetClipboardData(CF_UNICODETEXT, memory) != nullptr, "cannot seed clipboard");
    CloseClipboard();
    DestroyWindow(owner);
}

std::wstring ClipboardText() {
    Check(OpenClipboard(nullptr) != FALSE, "cannot open clipboard for verification");
    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    Check(data != nullptr, "Unicode clipboard data is missing");
    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
    Check(text != nullptr, "cannot lock clipboard data");
    const std::wstring result(text);
    GlobalUnlock(data);
    CloseClipboard();
    return result;
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
    Check((GetWindowLongPtrW(GetDlgItem(window, protocolui::ResultEdit), GWL_STYLE) & ES_READONLY) != 0,
          "protocol result edit is not read-only");

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

void CheckInteractions(HWND window) {
    PutClipboard(L"UNCHANGED");
    SetText(window, extension::EditFirst, L"SLOT-1");
    Click(window, protocolui::Copy);
    Message(window, WM_COMMAND, MAKEWPARAM(protocolui::FillSlotFirst, 0), 0);
    Check(ClipboardText() == L"UNCHANGED", "copy without a result changed clipboard");
    Check(Text(GetDlgItem(window, extension::EditFirst)) == L"SLOT-1",
          "fill without a result changed custom data");

    SelectFunction(window, 0);
    SetText(window, protocolui::SlaveEdit, L"01");
    SetText(window, protocolui::AddressEdit, L"0000");
    SetText(window, protocolui::QuantityEdit, L"0002");
    Click(window, protocolui::Generate);
    const std::wstring expected = L"01 03 00 00 00 02 C4 0B";
    Check(Text(GetDlgItem(window, protocolui::ResultEdit)) == expected,
          "03 generate button did not display the standard frame");
    Check(Text(GetDlgItem(window, protocolui::Status)).find(L"8 bytes / CRC C4 0B") !=
              std::wstring::npos,
          "generated frame status is incorrect");

    PutClipboard(L"OLD");
    Click(window, protocolui::Copy);
    const std::wstring copied = ClipboardText();
    Check(copied == expected, "copy did not place normalized frame on clipboard");

    for (int slot = 0; slot < kMaximumStoredCustomSlots; ++slot) {
        const auto sentinel = L"SLOT-" + std::to_wstring(slot + 1);
        SetText(window, extension::EditFirst + slot, sentinel.c_str());
    }
    SetText(window, kSendEditId, L"MAIN-UNCHANGED");
    const LRESULT rxBefore = Message(window, kQuery, 1);
    const LRESULT txBefore = Message(window, kQuery, 2);
    const LRESULT hexBefore = Message(GetDlgItem(window, kTxHexId), BM_GETCHECK);
    Message(window, WM_COMMAND, MAKEWPARAM(protocolui::FillSlotFirst + 4, 0), 0);
    for (int slot = 0; slot < kMaximumStoredCustomSlots; ++slot) {
        const auto expectedSlot = slot == 4 ? expected : L"SLOT-" + std::to_wstring(slot + 1);
        Check(Text(GetDlgItem(window, extension::EditFirst + slot)) == expectedSlot,
              "fill changed the wrong custom slot");
    }
    Check(Text(GetDlgItem(window, kSendEditId)) == L"MAIN-UNCHANGED",
          "fill changed the main send editor");
    Check(Message(window, kQuery, 1) == rxBefore && Message(window, kQuery, 2) == txBefore,
          "fill transmitted serial data");
    Check(Message(GetDlgItem(window, kTxHexId), BM_GETCHECK) == hexBefore,
          "fill changed the HEX send checkbox");
    Check(Text(GetDlgItem(window, protocolui::Status)) ==
              L"已填入自定义5；发送时请启用“十六进制发送”",
          "fill guidance is missing when HEX send is disabled");
    Message(window, WM_COMMAND,
            MAKEWPARAM(protocolui::FillSlotFirst + kMaximumStoredCustomSlots - 1, 0), 0);
    Check(Text(GetDlgItem(window, extension::EditFirst + kMaximumStoredCustomSlots - 1)) == expected,
          "fill command did not reach custom slot 16");
    Check(Message(window, kQuery, 2) == txBefore,
          "filling custom slot 16 transmitted serial data");

    SetText(window, protocolui::SlaveEdit, L"00");
    Click(window, protocolui::Generate);
    Check(Text(GetDlgItem(window, protocolui::ResultEdit)) == expected,
          "invalid generation cleared the previous valid result");
    Check(Text(GetDlgItem(window, protocolui::Status)).find(L"错误：") == 0,
          "invalid generation did not show an error status");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    TestProcess app;
    try {
        Start(app, argv[1]);
        CheckForm(app.window);
        CheckInteractions(app.window);
        std::cout << "Protocol UI tests passed. Checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << " (check " << checks << ")\n";
        return 1;
    }
}
