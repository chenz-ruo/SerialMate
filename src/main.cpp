#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dbt.h>
#include <richedit.h>
#include <shellapi.h>
#include <objbase.h>

#include "LogWriter.h"
#include "Product.h"
#include "resource.h"
#include "SerialPort.h"
#include "UpdateChecker.h"
#include "UpdateInstaller.h"
#include "Utilities.h"
#include "Version.h"
#include "CommRecord.h"
#include "CommView.h"
#include "ConfigStore.h"
#include "TextCodec.h"
#include "UiGeometry.h"
#include "ExtensionControlIds.h"
#include "SerialPortInfo.h"
#include "RxIngressQueue.h"
#include "ProtocolGenerator.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <cwctype>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr const wchar_t* kWindowClass = product::kMainWindowClass;
constexpr UINT WM_SERIAL_DATA = WM_APP + 1;
constexpr UINT WM_SERIAL_ERROR = WM_APP + 2;
constexpr UINT WM_UPDATE_FOUND = WM_APP + 3;
constexpr UINT WM_UPDATE_READY = WM_APP + 4;
constexpr UINT WM_TEST_QUERY = WM_APP + 100;
constexpr UINT_PTR TIMER_SEND = 1;
constexpr UINT_PTR TIMER_STATS = 2;
constexpr UINT_PTR TIMER_RECORD_VIEW = 3;

void CenterWindowOnMonitor(HWND window) {
    RECT windowRect{};
    if (!GetWindowRect(window, &windowRect)) return;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!GetMonitorInfoW(monitor, &monitorInfo)) return;

    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    const RECT& work = monitorInfo.rcWork;
    const int x = work.left + ((work.right - work.left) - width) / 2;
    const int y = work.top + ((work.bottom - work.top) - height) / 2;
    SetWindowPos(window, nullptr, x, y, 0, 0,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
}

enum ControlId : int {
    ID_OPEN = 100, ID_CLOSE, ID_NEW, ID_LOG, ID_ABOUT,
    ID_PORT, ID_BAUD, ID_DATA_BITS, ID_STOP_BITS, ID_PARITY, ID_FLOW,
    ID_TIMESTAMP, ID_AUTOLINE, ID_SIMPLE_MODE, ID_RX_ONLY, ID_RX_HEX, ID_RX_FILE,
    ID_TX_HEX, ID_TX_CR, ID_TX_LF, ID_TIMED, ID_INTERVAL, ID_SEND_TIMED,
    ID_COPY_ALL, ID_CLEAR_LOG, ID_PAUSE, ID_EXPORT, ID_LOG_VIEW,
    ID_SEND_EDIT, ID_SEND, ID_CLEAR_SEND, ID_LOAD_FILE,
    ID_STATUS_LEFT, ID_STATUS_RIGHT, ID_ENCODING,
    ID_COPY_FULL = 500, ID_COPY_HEX, ID_COPY_TEXT,
    ID_COPY_SELECTED_FULL, ID_COPY_SELECTED_HEX, ID_COPY_SELECTED_TEXT
};

struct SerialFailure { DWORD code; std::wstring message; };

COLORREF MixColor(COLORREF a, COLORREF b, int percentB) {
    const int percentA = 100 - percentB;
    return RGB((GetRValue(a) * percentA + GetRValue(b) * percentB) / 100,
               (GetGValue(a) * percentA + GetGValue(b) * percentB) / 100,
               (GetBValue(a) * percentA + GetBValue(b) * percentB) / 100);
}

std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

std::string RecorderLine(const comm::Record& record) {
    const std::wstring line = L"[" + record.timestamp + L"] " +
        (record.direction == comm::Direction::Rx ? L"← RX " : L"→ TX ") +
        util::FormatBytes(record.rawBytes) + L"\r\n";
    const auto bytes = util::Utf8Bytes(line);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool IsHexDigit(wchar_t ch) {
    return (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F') || (ch >= L'a' && ch <= L'f');
}

std::wstring NormalizeHexEditorText(const std::wstring& input) {
    std::wstring compact;
    compact.reserve(input.size());
    for (wchar_t ch : input) if (IsHexDigit(ch)) compact.push_back(static_cast<wchar_t>(towupper(ch)));
    std::wstring result;
    result.reserve(compact.size() + compact.size() / 2);
    for (std::size_t i = 0; i < compact.size(); ++i) {
        if (i && (i % 2) == 0) result.push_back(L' ');
        result.push_back(compact[i]);
    }
    return result;
}

bool PutClipboardText(HWND owner, const std::wstring& text) {
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    auto* target = static_cast<wchar_t*>(GlobalLock(memory));
    if (!target) { GlobalFree(memory); return false; }
    memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) { GlobalFree(memory); return false; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, memory)) { GlobalFree(memory); CloseClipboard(); return false; }
    CloseClipboard();
    return true;
}

void SetFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

std::vector<std::wstring> EnumeratePorts() {
    std::vector<std::wstring> ports;
    for (const auto& info : EnumerateSerialPorts()) ports.push_back(info.portName);
    return ports;
}

std::wstring ChoosePath(HWND owner, bool save, const wchar_t* title, const wchar_t* filter,
                        const wchar_t* extension = nullptr) {
    std::array<wchar_t, 32768> file{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrTitle = title;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.lpstrDefExt = extension;
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    return ok ? std::wstring(file.data()) : std::wstring();
}

void DrawFlatEditBorder(HWND window) {
    HDC dc = GetWindowDC(window);
    if (!dc) return;
    RECT rect{};
    GetWindowRect(window, &rect);
    OffsetRect(&rect, -rect.left, -rect.top);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(205, 215, 225));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, 0, 0, rect.right, rect.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    ReleaseDC(window, dc);
}

LRESULT CALLBACK FlatEditSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR subclassId, DWORD_PTR) {
    if (message == WM_PAINT || message == WM_NCPAINT) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        DrawFlatEditBorder(window);
        return result;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, FlatEditSubclass, subclassId);
    return DefSubclassProc(window, message, wParam, lParam);
}

void CenterSingleLineEditText(HWND window) {
    RECT client{};
    if (!GetClientRect(window, &client)) return;
    HDC dc = GetDC(window);
    if (!dc) return;
    const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    const HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW metrics{};
    const bool measured = GetTextMetricsW(dc, &metrics) != FALSE;
    if (oldFont) SelectObject(dc, oldFont);
    ReleaseDC(window, dc);
    if (!measured) return;
    const int height = client.bottom - client.top;
    const int lineHeight = metrics.tmHeight + metrics.tmExternalLeading;
    const int verticalInset = std::max(2, (height - lineHeight) / 2);
    RECT format{client.left + 2, client.top + verticalInset,
                std::max(client.left + 2, client.right - 2),
                std::max(client.top + verticalInset, client.bottom - verticalInset)};
    SendMessageW(window, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&format));
}

LRESULT CALLBACK IntervalEditSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR subclassId, DWORD_PTR) {
    if (message == WM_CHAR && (wParam == L'\r' || wParam == L'\n')) return 0;
    if (message == WM_SIZE || message == WM_SETFONT) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        CenterSingleLineEditText(window);
        return result;
    }
    if (message == WM_PAINT || message == WM_NCPAINT) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        DrawFlatEditBorder(window);
        return result;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, IntervalEditSubclass, subclassId);
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK CustomEditSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR subclassId, DWORD_PTR) {
    const HWND parent = GetParent(window);
    const bool hexMode = parent && IsDlgButtonChecked(parent, ID_TX_HEX) == BST_CHECKED;
    if (message == WM_CHAR) {
        if (wParam == L'\r' || wParam == L'\n') return 0;
        if (hexMode && wParam >= 0x20 && !IsHexDigit(static_cast<wchar_t>(wParam))) return 0;
    }
    if (hexMode && message == WM_PASTE) {
        if (!OpenClipboard(window)) return 0;
        std::wstring pasted;
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
                pasted = text;
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        const std::wstring normalized = NormalizeHexEditorText(pasted);
        SendMessageW(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(normalized.c_str()));
        return 0;
    }
    if (message == WM_SIZE || message == WM_SETFONT || message == WM_SHOWWINDOW) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        CenterSingleLineEditText(window);
        return result;
    }
    if (message == WM_PAINT || message == WM_NCPAINT) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        DrawFlatEditBorder(window);
        return result;
    }
    if (!hexMode && message == WM_CHAR) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        return result;
    }
    if (message == WM_KEYUP && (wParam == VK_BACK || wParam == VK_DELETE)) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        return result;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, CustomEditSubclass, subclassId);
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK SendEditSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR subclassId, DWORD_PTR) {
    HWND parent = GetParent(window);
    const bool hexMode = parent && IsDlgButtonChecked(parent, ID_TX_HEX) == BST_CHECKED;
    if (message == WM_PAINT || message == WM_NCPAINT) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        DrawFlatEditBorder(window);
        return result;
    }
    if (hexMode && message == WM_CHAR) {
        if (wParam >= 0x20 && !IsHexDigit(static_cast<wchar_t>(wParam))) return 0;
    }
    if (hexMode && message == WM_PASTE) {
        if (!OpenClipboard(window)) return 0;
        std::wstring pasted;
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
                pasted = text; GlobalUnlock(data);
            }
        }
        CloseClipboard();
        const std::wstring normalized = NormalizeHexEditorText(pasted);
        SendMessageW(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(normalized.c_str()));
        return 0;
    }
    if (!hexMode && message == WM_CHAR) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        return result;
    }
    if (hexMode && message == WM_KEYUP && (wParam == VK_BACK || wParam == VK_DELETE)) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        const std::wstring normalized = NormalizeHexEditorText(WindowText(window));
        SetWindowTextW(window, normalized.c_str());
        SendMessageW(window, EM_SETSEL, normalized.size(), normalized.size());
        // SetWindowText can replace the edit's internal text without causing
        // an immediate erase on some native edit controls. Force the control
        // to invalidate and repaint so deleted glyphs cannot remain as stale
        // pixels in the send area.
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        return result;
    }
    if (!hexMode && message == WM_KEYUP && (wParam == VK_BACK || wParam == VK_DELETE)) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        return result;
    }
    if (message == WM_KEYDOWN && wParam == VK_RETURN && (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
        SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(ID_SEND, BN_CLICKED), reinterpret_cast<LPARAM>(window));
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, SendEditSubclass, subclassId);
    return DefSubclassProc(window, message, wParam, lParam);
}

class Application final {
public:
    explicit Application(HWND window) : window_(window), started_(std::chrono::steady_clock::now()) {}
    ~Application() { Shutdown(); }

    void Create();
    void Layout();
    int DefaultClientWidth() const { return layout_.defaultClientWidth; }
    void Command(int id, int notification, HWND sender);
    void Timer(UINT_PTR id);
    void DeviceChanged();
    void DrainReceive();
    void SerialError(std::unique_ptr<SerialFailure> failure);
    void UpdateFound(std::unique_ptr<UpdateManifest> manifest);
    void UpdateReady(std::unique_ptr<UpdateDownloadResult> result);
    void DrawButton(DRAWITEMSTRUCT* draw);
    void Paint();
    HBRUSH ControlColor(HDC dc, HWND control, UINT message);
    void Shutdown();
    LRESULT QueryState(WPARAM query) const;
    void SetTestValue(ULONG_PTR key, const wchar_t* value);

private:
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle, int id);
    HWND Label(const wchar_t* text, int id = 0);
    HWND Button(const wchar_t* text, int id, bool primary = false);
    HWND Check(const wchar_t* text, int id);
    HWND Combo(int id);
    void AddComboItems(HWND combo, std::initializer_list<const wchar_t*> items, int selected);
    void ScanPorts(bool preserve);
    void LoadConfiguration();
    void SaveConfiguration();
    void SetTextEncoding(textcodec::TextEncoding encoding);
    void FlushRxPending();
    void OpenPort();
    void ClosePort(bool showRecord = true);
    bool BuildSendDataFromText(const std::wstring& text, std::vector<std::uint8_t>& data,
                               bool fromTimer = false, int customSlot = -1);
    void SendText(const std::wstring& text, bool fromTimer = false, int customSlot = -1);
    void SendData(bool fromTimer = false);
    void StopTimedSendOnError(const std::wstring& reason);
    void ToggleHexEditorMode();
    void NormalizeHexEditor();
    void NormalizeCustomHexEditor(int slot);
    void LayoutProtocolControls();
    void UpdateProtocolForm();
    protocol::Function SelectedProtocolFunction() const;
    void SetProtocolControlsVisible(bool visible);
    void GenerateProtocolFrame();
    void CopyProtocolResult();
    void ShowProtocolFillMenu();
    void FillProtocolResultIntoCustomSlot(int slot);
    void UpdateProtocolStatus(const std::wstring& text);
    void AppendRecord(bool receive, const std::vector<std::uint8_t>& data);
    void CopyRecords(int mode);
    void ShowCopyMenu();
    void AppendSystem(const std::wstring& message, bool error = false);
    void UpdateStatus();
    void ToggleLogging();
    void ToggleRecorder();
    void ExportLog();
    bool ExportLogPath(const std::wstring& path);
    bool LoadFilePath(const std::wstring& path);
    bool Checked(int id) const;
    int ComboSelection(int id) const;
    void SetConnectedUi(bool connected);
    HWND Get(int id) const { return GetDlgItem(window_, id); }

    HWND window_{};
    HFONT font_{};
    HFONT titleFont_{};
    HBRUSH whiteBrush_{};
    HBRUSH backgroundBrush_{};
    SerialPort serial_;
    std::shared_ptr<LogWriter> logWriter_ = std::make_shared<LogWriter>();
    std::shared_ptr<LogWriter> recorder_ = std::make_shared<LogWriter>();
    std::shared_ptr<RxIngressQueue> rxIngress_ = std::make_shared<RxIngressQueue>();
    std::shared_ptr<std::atomic_int> encodingChoice_ = std::make_shared<std::atomic_int>(0);
    UpdateChecker updateChecker_;
    std::wstring structuredLogPath_;
    std::uint64_t rxBytes_ = 0;
    std::uint64_t txBytes_ = 0;
    bool paused_ = false;
    bool timedSendBlocked_ = false;
    bool disconnectHandled_ = true;
    bool shuttingDown_ = false;
    std::chrono::steady_clock::time_point started_;
    std::vector<SerialPortInfo> portInfos_;
    std::wstring connectedPort_;
    textcodec::TextEncoding encoding_ = textcodec::TextEncoding::Utf8;
    textcodec::RxTextDecoder rxDecoder_{encoding_};
    std::wstring rxPendingTimestamp_;
    comm::RecordBuffer records_;
    comm::RecordView recordView_{records_};
    bool normalizingHex_ = false;
    bool normalizingCustomHex_ = false;
    std::vector<std::uint8_t> sendRawSnapshot_;
    MainLayoutGeometry layout_{};
    std::unique_ptr<config::ConfigStore> configStore_;
    config::ConfigData customData_{};
    std::bitset<config::kSlotCount> customDataDirty_{};
    bool loadingConfiguration_ = false;
    bool configurationSaved_ = false;
    protocol::Result protocolResult_{};
};

HWND Application::Make(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle, int id) {
    HWND control = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 100, 30,
                                   window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    SetFont(control, font_);
    return control;
}

HWND Application::Label(const wchar_t* text, int id) {
    return Make(L"STATIC", text, SS_LEFT | SS_CENTERIMAGE, 0, id);
}

HWND Application::Button(const wchar_t* text, int id, bool primary) {
    HWND button = Make(L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, 0, id);
    SetWindowLongPtrW(button, GWLP_USERDATA, primary ? 1 : 0);
    return button;
}

HWND Application::Check(const wchar_t* text, int id) {
    return Make(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, 0, id);
}

HWND Application::Combo(int id) {
    return Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, id);
}

void Application::AddComboItems(HWND combo, std::initializer_list<const wchar_t*> items, int selected) {
    for (const auto* item : items) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

void Application::Create() {
    font_ = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH, L"Microsoft YaHei UI");
    titleFont_ = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
    whiteBrush_ = CreateSolidBrush(RGB(255, 255, 255));
    backgroundBrush_ = CreateSolidBrush(RGB(244, 248, 252));

    Button(L"打开串口", ID_OPEN, true);
    Button(L"关闭串口", ID_CLOSE);
    Button(L"新建窗口", ID_NEW);
    Button(L"启动日志", ID_LOG);
    Button(L"关于", ID_ABOUT);

    Label(L"串口设置", 9001); SetFont(Get(9001), titleFont_);
    Label(L"串口号"); Combo(ID_PORT);
    Label(L"波特率"); auto baud = Combo(ID_BAUD);
    AddComboItems(baud, {L"1200", L"2400", L"4800", L"9600", L"14400", L"19200", L"28800", L"38400", L"57600", L"115200", L"230400", L"460800", L"921600"}, 9);
    Label(L"数据位"); auto dataBits = Combo(ID_DATA_BITS); AddComboItems(dataBits, {L"5", L"6", L"7", L"8"}, 3);
    Label(L"停止位"); auto stopBits = Combo(ID_STOP_BITS); AddComboItems(stopBits, {L"1", L"1.5", L"2"}, 0);
    Label(L"校验位"); auto parity = Combo(ID_PARITY); AddComboItems(parity, {L"None", L"Odd", L"Even", L"Mark", L"Space"}, 0);
    Label(L"流控制"); auto flow = Combo(ID_FLOW); AddComboItems(flow, {L"None", L"RTS/CTS"}, 0);

    Label(L"接收设置", 9002); SetFont(Get(9002), titleFont_);
    Check(L"十六进制显示", ID_RX_HEX);
    Check(L"显示时间戳", ID_TIMESTAMP);
    Check(L"自动滚动", ID_AUTOLINE);
    Check(L"接收独显", ID_RX_ONLY);
    Check(L"传统古法", ID_SIMPLE_MODE);

    Label(L"发送设置", 9003); SetFont(Get(9003), titleFont_);
    Check(L"十六进制发送", ID_TX_HEX); Check(L"发送新行 (CR)", ID_TX_CR);
    Check(L"发送新行 (LF)", ID_TX_LF); Check(L"定时发送", ID_TIMED);
    auto interval = Make(L"EDIT", L"1000",
                         ES_NUMBER | ES_CENTER | ES_MULTILINE | ES_AUTOHSCROLL | WS_TABSTOP,
                         0, ID_INTERVAL);
    SetWindowSubclass(interval, IntervalEditSubclass, 1, 0);
    CenterSingleLineEditText(interval);
    SendMessageW(interval, EM_SETLIMITTEXT, 7, 0); Label(L"ms", 9010);
    Label(L"文本编码（发送/显示）", 9011); auto encoding = Combo(ID_ENCODING);
    AddComboItems(encoding, {textcodec::Name(textcodec::TextEncoding::Utf8),
                             textcodec::Name(textcodec::TextEncoding::Gbk),
                             textcodec::Name(textcodec::TextEncoding::Ascii)}, 0);

    Label(L"通信记录", 9004); SetFont(Get(9004), titleFont_);
    Button(L"实时记录", ID_RX_FILE); Button(L"暂停显示", ID_PAUSE);
    Button(L"复制 ▼", ID_COPY_ALL); Button(L"清空", ID_CLEAR_LOG);
    Button(L"导出...", ID_EXPORT);
    recordView_.Create(window_, ID_LOG_VIEW);
    Label(L"自定义数据", extension::CustomTitle); SetFont(Get(extension::CustomTitle), titleFont_);
    Label(L"协议数据生成", extension::ProtocolTitle); SetFont(Get(extension::ProtocolTitle), titleFont_);
    for (int id : {extension::CustomTitle, extension::ProtocolTitle})
        SetWindowLongPtrW(Get(id), GWL_STYLE, GetWindowLongPtrW(Get(id), GWL_STYLE) | SS_ENDELLIPSIS);
    ShowWindow(Get(extension::CustomTitle), SW_HIDE);
    ShowWindow(Get(extension::ProtocolTitle), SW_HIDE);

    Label(L"数据发送", 9005); SetFont(Get(9005), titleFont_);
    auto sendEdit = Make(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
                         0, ID_SEND_EDIT);
    SetFont(sendEdit, recordView_.Font());
    SetWindowLongPtrW(sendEdit, GWLP_USERDATA, 1);
    SetWindowSubclass(sendEdit, SendEditSubclass, 1, 0);
    Button(L"发送（Enter）", ID_SEND, true); Button(L"加载文件...", ID_LOAD_FILE);
    Button(L"定时发送", ID_SEND_TIMED); Button(L"清空", ID_CLEAR_SEND);
    // Create every slot once. Its HWND and text survive all layout transitions.
    for (int i = 0; i < kMaximumStoredCustomSlots; ++i) {
        const auto number = std::to_wstring(i + 1);
        HWND index = Label(number.c_str(), extension::IndexFirst + i);
        SetWindowLongPtrW(index, GWL_STYLE, GetWindowLongPtrW(index, GWL_STYLE) | SS_CENTERIMAGE);
        HWND edit = Make(L"EDIT", L"", ES_MULTILINE | ES_AUTOHSCROLL | WS_TABSTOP,
                         0, extension::EditFirst + i);
        SetWindowSubclass(edit, CustomEditSubclass, 1, 0);
        CenterSingleLineEditText(edit);
        HWND send = Button(L"发送", extension::SendFirst + i);
        SendMessageW(edit, EM_SETLIMITTEXT, 4096, 0);
        ShowWindow(index, SW_HIDE);
        ShowWindow(edit, SW_HIDE);
        ShowWindow(send, SW_HIDE);
    }
    Label(L"协议类型", protocolui::TypeLabel);
    auto protocolType = Combo(protocolui::TypeCombo);
    AddComboItems(protocolType, {L"Modbus RTU"}, 0);
    Label(L"从机地址", protocolui::SlaveLabel);
    auto slave = Make(L"EDIT", L"01", ES_MULTILINE | ES_AUTOHSCROLL | WS_TABSTOP,
                      0, protocolui::SlaveEdit);
    SetWindowSubclass(slave, IntervalEditSubclass, 1, 0);
    CenterSingleLineEditText(slave);
    Label(L"功能码", protocolui::FunctionLabel);
    auto function = Combo(protocolui::FunctionCombo);
    AddComboItems(function, {L"03 读保持寄存器", L"04 读输入寄存器",
                             L"06 写单个寄存器", L"10 写多个寄存器"}, 0);
    Label(L"起始地址", protocolui::AddressLabel);
    auto address = Make(L"EDIT", L"0000", ES_MULTILINE | ES_AUTOHSCROLL | WS_TABSTOP,
                        0, protocolui::AddressEdit);
    Label(L"寄存器数量", protocolui::QuantityLabel);
    auto quantity = Make(L"EDIT", L"0001", ES_MULTILINE | ES_AUTOHSCROLL | WS_TABSTOP,
                         0, protocolui::QuantityEdit);
    Label(L"写入数据", protocolui::DataLabel);
    auto data = Make(L"EDIT", L"0000", ES_MULTILINE | ES_AUTOHSCROLL | WS_TABSTOP,
                     0, protocolui::DataEdit);
    for (HWND edit : {address, quantity, data}) {
        SetWindowSubclass(edit, IntervalEditSubclass, 1, 0);
        CenterSingleLineEditText(edit);
        SendMessageW(edit, EM_SETLIMITTEXT, 1024, 0);
    }
    SendMessageW(slave, EM_SETLIMITTEXT, 2, 0);
    Button(L"生成", protocolui::Generate, true);
    Button(L"填入自定义...", protocolui::FillCustom);
    Label(L"生成结果", protocolui::ResultLabel);
    auto result = Make(L"EDIT", L"", ES_MULTILINE | ES_AUTOHSCROLL | ES_READONLY,
                       0, protocolui::ResultEdit);
    SetWindowSubclass(result, IntervalEditSubclass, 1, 0);
    CenterSingleLineEditText(result);
    Button(L"复制", protocolui::Copy);
    Label(L"状态：等待生成", protocolui::Status);
    SetProtocolControlsVisible(false);
    UpdateProtocolForm();
    LoadConfiguration();
    Label(L"●  未连接", ID_STATUS_LEFT);
    auto statusRight = Label(L"RX: 0    TX: 0", ID_STATUS_RIGHT);
    SetWindowLongPtrW(statusRight, GWL_STYLE, GetWindowLongPtrW(statusRight, GWL_STYLE) | SS_RIGHT);

    CheckDlgButton(window_, ID_RX_HEX, BST_CHECKED);
    CheckDlgButton(window_, ID_TIMESTAMP, BST_CHECKED);
    CheckDlgButton(window_, ID_AUTOLINE, BST_CHECKED);
    CheckDlgButton(window_, ID_RX_ONLY, BST_UNCHECKED);
    CheckDlgButton(window_, ID_SIMPLE_MODE, BST_UNCHECKED);
    CheckDlgButton(window_, ID_TX_CR, BST_UNCHECKED);
    SetTextEncoding(textcodec::TextEncoding::Utf8);
    rxIngress_->SetNotifier([hwnd = window_] {
        return PostMessageW(hwnd, WM_SERIAL_DATA, 0, 0) != FALSE;
    });
    ScanPorts(false);
    SetConnectedUi(false);
    SetTimer(window_, TIMER_STATS, 500, nullptr);
    SetTimer(window_, TIMER_RECORD_VIEW, 33, nullptr);
    recordView_.SetTimestamps(Checked(ID_TIMESTAMP));
    Layout();
    updateChecker_.Start([hwnd = window_](UpdateManifest manifest) {
        auto* message = new UpdateManifest(std::move(manifest));
        if (!PostMessageW(hwnd, WM_UPDATE_FOUND, 0, reinterpret_cast<LPARAM>(message))) delete message;
    });
}

protocol::Function Application::SelectedProtocolFunction() const {
    switch (ComboSelection(protocolui::FunctionCombo)) {
    case 1: return protocol::Function::ReadInputRegisters;
    case 2: return protocol::Function::WriteSingleRegister;
    case 3: return protocol::Function::WriteMultipleRegisters;
    default: return protocol::Function::ReadHoldingRegisters;
    }
}

void Application::SetProtocolControlsVisible(bool visible) {
    const bool showData = visible &&
                          SelectedProtocolFunction() == protocol::Function::WriteMultipleRegisters;
    const std::array<int, 18> ids{
        protocolui::TypeLabel, protocolui::TypeCombo, protocolui::SlaveLabel,
        protocolui::SlaveEdit, protocolui::FunctionLabel, protocolui::FunctionCombo,
        protocolui::AddressLabel, protocolui::AddressEdit, protocolui::QuantityLabel,
        protocolui::QuantityEdit, protocolui::DataLabel, protocolui::DataEdit,
        protocolui::Generate, protocolui::FillCustom, protocolui::ResultLabel,
        protocolui::ResultEdit, protocolui::Copy, protocolui::Status};
    for (int id : ids) {
        HWND control = Get(id);
        const bool show = visible && (id != protocolui::DataLabel && id != protocolui::DataEdit ||
                                      showData);
        if (!show && GetFocus() == control) SetFocus(Get(ID_SEND_EDIT));
        ShowWindow(control, show ? SW_SHOWNA : SW_HIDE);
    }
}

void Application::LayoutProtocolControls() {
    const RECT card = layout_.protocolCard;
    if (IsRectEmpty(&card)) return;
    const UINT dpi = GetDpiForWindow(window_);
    const auto scale = [dpi](int value) { return std::max(1, MulDiv(value, static_cast<int>(dpi), 96)); };
    const auto move = [&](int id, int left, int top, int width, int height) {
        MoveWindow(Get(id), left, top, std::max(1, width), std::max(1, height), TRUE);
    };
    const int left = card.left + layout_.cardPadding;
    const int right = card.right - layout_.cardPadding;
    const int width = right - left;
    const int columnGap = scale(6);
    int y = layout_.protocolTitle.bottom +
            (layout_.settings.serialFields[0].top - layout_.settings.serialTitle.bottom);
    const int rowGap = std::max(2, MulDiv(4, layout_.settings.scalePercent, 100));
    constexpr int maximumRows = 8;
    const int availableHeight = std::max(1, static_cast<int>(card.bottom) - layout_.cardPadding - y);
    const int fittedRowHeight = std::max(1,
        (availableHeight - (maximumRows - 1) * rowGap) / maximumRows);
    const int rowHeight = std::min(
        static_cast<int>(layout_.settings.interval.bottom - layout_.settings.interval.top),
        fittedRowHeight);

    const int typeLabelWidth = scale(66);
    move(protocolui::TypeLabel, left, y, typeLabelWidth, rowHeight);
    move(protocolui::TypeCombo, left + typeLabelWidth, y, width - typeLabelWidth, rowHeight);
    y += rowHeight + rowGap;

    const int slaveLabelWidth = scale(62);
    const int slaveEditWidth = scale(44);
    const int functionLabelWidth = scale(54);
    move(protocolui::SlaveLabel, left, y, slaveLabelWidth, rowHeight);
    move(protocolui::SlaveEdit, left + slaveLabelWidth, y, slaveEditWidth, rowHeight);
    const int functionLabelX = left + slaveLabelWidth + slaveEditWidth + columnGap;
    move(protocolui::FunctionLabel, functionLabelX, y, functionLabelWidth, rowHeight);
    move(protocolui::FunctionCombo, functionLabelX + functionLabelWidth, y,
         right - functionLabelX - functionLabelWidth, rowHeight);
    y += rowHeight + rowGap;

    const int addressLabelWidth = scale(66);
    const int addressEditWidth = scale(58);
    const int quantityLabelWidth = scale(76);
    move(protocolui::AddressLabel, left, y, addressLabelWidth, rowHeight);
    move(protocolui::AddressEdit, left + addressLabelWidth, y, addressEditWidth, rowHeight);
    const int quantityLabelX = left + addressLabelWidth + addressEditWidth + columnGap;
    move(protocolui::QuantityLabel, quantityLabelX, y, quantityLabelWidth, rowHeight);
    move(protocolui::QuantityEdit, quantityLabelX + quantityLabelWidth, y,
         right - quantityLabelX - quantityLabelWidth, rowHeight);
    y += rowHeight + rowGap;

    if (SelectedProtocolFunction() == protocol::Function::WriteMultipleRegisters) {
        move(protocolui::DataLabel, left, y, addressLabelWidth, rowHeight);
        move(protocolui::DataEdit, left + addressLabelWidth, y, width - addressLabelWidth, rowHeight);
        y += rowHeight + rowGap;
    }

    const int buttonWidth = (width - columnGap) / 2;
    move(protocolui::Generate, left, y, buttonWidth, rowHeight);
    move(protocolui::FillCustom, left + buttonWidth + columnGap, y,
         width - buttonWidth - columnGap, rowHeight);
    y += rowHeight + rowGap;
    move(protocolui::ResultLabel, left, y, width, rowHeight);
    y += rowHeight + rowGap;
    const int copyWidth = scale(56);
    move(protocolui::ResultEdit, left, y, width - copyWidth - columnGap, rowHeight);
    move(protocolui::Copy, right - copyWidth, y, copyWidth, rowHeight);
    y += rowHeight + rowGap;
    move(protocolui::Status, left, y, width, rowHeight);

    for (int id : {protocolui::SlaveEdit, protocolui::AddressEdit, protocolui::QuantityEdit,
                   protocolui::DataEdit, protocolui::ResultEdit})
        CenterSingleLineEditText(Get(id));
}

void Application::UpdateProtocolForm() {
    const auto function = SelectedProtocolFunction();
    SetWindowTextW(Get(protocolui::AddressLabel),
                   function == protocol::Function::WriteSingleRegister ? L"寄存器地址" : L"起始地址");
    SetWindowTextW(Get(protocolui::QuantityLabel),
                   function == protocol::Function::WriteSingleRegister ? L"写入值" : L"寄存器数量");
    LayoutProtocolControls();
    SetProtocolControlsVisible(layout_.extensionVisible);
}

void Application::UpdateProtocolStatus(const std::wstring& text) {
    SetWindowTextW(Get(protocolui::Status), text.c_str());
}

void Application::GenerateProtocolFrame() {
    protocol::Request request;
    request.function = SelectedProtocolFunction();
    request.slave = WindowText(Get(protocolui::SlaveEdit));
    request.address = WindowText(Get(protocolui::AddressEdit));
    if (request.function == protocol::Function::WriteSingleRegister)
        request.value = WindowText(Get(protocolui::QuantityEdit));
    else
        request.quantity = WindowText(Get(protocolui::QuantityEdit));
    if (request.function == protocol::Function::WriteMultipleRegisters)
        request.data = WindowText(Get(protocolui::DataEdit));

    auto generated = protocol::Generate(request);
    if (!generated) {
        UpdateProtocolStatus(L"错误：" + generated.error);
        return;
    }
    protocolResult_ = std::move(generated);
    SetWindowTextW(Get(protocolui::ResultEdit), protocolResult_.hex.c_str());
    const std::wstring crc = util::FormatBytes(
        std::vector<std::uint8_t>{protocolResult_.crcLow, protocolResult_.crcHigh});
    UpdateProtocolStatus(L"状态：" + std::to_wstring(protocolResult_.frame.size()) +
                         L" bytes / CRC " + crc);
}

void Application::CopyProtocolResult() {
    if (!protocolResult_) {
        UpdateProtocolStatus(L"请先生成协议数据");
        return;
    }
    if (PutClipboardText(window_, protocolResult_.hex))
        UpdateProtocolStatus(L"已复制生成结果");
    else
        UpdateProtocolStatus(L"错误：无法访问剪贴板");
}

void Application::FillProtocolResultIntoCustomSlot(int slot) {
    if (!protocolResult_) {
        UpdateProtocolStatus(L"请先生成协议数据");
        return;
    }
    if (slot < 0 || slot >= kMaximumStoredCustomSlots) return;
    SetWindowTextW(Get(extension::EditFirst + slot), protocolResult_.hex.c_str());
    const std::wstring target = L"已填入自定义" + std::to_wstring(slot + 1);
    UpdateProtocolStatus(Checked(ID_TX_HEX)
        ? target
        : target + L"；发送时请启用“十六进制发送”");
}

void Application::ShowProtocolFillMenu() {
    if (!protocolResult_) {
        UpdateProtocolStatus(L"请先生成协议数据");
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        UpdateProtocolStatus(L"错误：无法创建槽位菜单");
        return;
    }
    for (int slot = 0; slot < kMaximumStoredCustomSlots; ++slot) {
        const std::wstring text = L"自定义" + std::to_wstring(slot + 1);
        AppendMenuW(menu, MF_STRING, protocolui::FillSlotFirst + slot, text.c_str());
    }
    RECT button{};
    GetWindowRect(Get(protocolui::FillCustom), &button);
    const int selected = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON |
                                              TPM_RETURNCMD,
                                        button.left, button.bottom, 0, window_, nullptr);
    DestroyMenu(menu);
    if (selected >= protocolui::FillSlotFirst &&
        selected < protocolui::FillSlotFirst + kMaximumStoredCustomSlots)
        FillProtocolResultIntoCustomSlot(selected - protocolui::FillSlotFirst);
}

void Application::Layout() {
    RECT client{}; GetClientRect(window_, &client);
    const int width = client.right;
    const int height = client.bottom;
    const UINT dpi = GetDpiForWindow(window_);
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    recordView_.SetDpi(dpi);
    layout_ = CalculateMainLayoutGeometry(width, height, dpi, recordView_.MinimumContentWidth8());
    auto& geometry = layout_;
    auto& settingsGeometry_ = layout_.settings;
    const auto& logCard_ = layout_.commRecordCard;
    const auto& sendCard_ = layout_.dataSendCard;
    const int gap = geometry.horizontalGap;
    const int rightX = geometry.rightX;
    const int rightW = geometry.rightWidth;
    // RecordView owns the canonical monospace HFONT and recreates it when the
    // DPI changes. Re-apply that same handle to the send editor on every
    // layout pass so both text areas stay identical after a DPI transition.
    if (Get(ID_SEND_EDIT) && recordView_.Font()) SetFont(Get(ID_SEND_EDIT), recordView_.Font());
    const auto moveRect = [&](HWND control, const RECT& rect) {
        if (!control) return;
        MoveWindow(control, rect.left, rect.top, rect.right - rect.left,
                   rect.bottom - rect.top, TRUE);
    };
    moveRect(Get(ID_OPEN), geometry.openButton);
    moveRect(Get(ID_CLOSE), geometry.closeButton);
    moveRect(Get(ID_NEW), geometry.newButton);
    moveRect(Get(ID_LOG), geometry.logButton);
    moveRect(Get(ID_ABOUT), geometry.aboutButton);
    moveRect(Get(9001), settingsGeometry_.serialTitle);
    const std::array<int, 6> comboIds{ID_PORT, ID_BAUD, ID_DATA_BITS, ID_STOP_BITS, ID_PARITY, ID_FLOW};
    const std::array<const wchar_t*, 6> labels{L"串口号", L"波特率", L"数据位", L"停止位", L"校验位", L"流控制"};
    for (std::size_t index = 0; index < comboIds.size(); ++index) {
        HWND combo = Get(comboIds[index]);
        moveRect(combo, settingsGeometry_.serialFields[index]);
        // A native dropdown chooses its collapsed height from its font and
        // theme, not the requested MoveWindow height. Align the label to the
        // actual visible bounds after sizing the dropdown.
        RECT visibleField{};
        if (GetWindowRect(combo, &visibleField)) {
            MapWindowPoints(HWND_DESKTOP, window_, reinterpret_cast<POINT*>(&visibleField), 2);
            settingsGeometry_.serialLabels[index].top = visibleField.top;
            settingsGeometry_.serialLabels[index].bottom = visibleField.bottom;
        }
        moveRect(FindWindowExW(window_, nullptr, L"STATIC", labels[index]),
                 settingsGeometry_.serialLabels[index]);
    }
    moveRect(Get(9002), settingsGeometry_.receiveTitle);
    moveRect(Get(ID_RX_HEX), settingsGeometry_.receiveHex);
    moveRect(Get(ID_TIMESTAMP), settingsGeometry_.timestamp);
    moveRect(Get(ID_AUTOLINE), settingsGeometry_.autoScroll);
    moveRect(Get(ID_RX_ONLY), settingsGeometry_.receiveOnly);
    moveRect(Get(ID_SIMPLE_MODE), settingsGeometry_.simpleMode);
    moveRect(Get(9003), settingsGeometry_.sendTitle);
    moveRect(Get(ID_TX_HEX), settingsGeometry_.txHex);
    moveRect(Get(ID_TX_CR), settingsGeometry_.txCr);
    moveRect(Get(ID_TX_LF), settingsGeometry_.txLf);
    moveRect(Get(ID_TIMED), settingsGeometry_.timed);
    moveRect(Get(ID_INTERVAL), settingsGeometry_.interval);
    moveRect(Get(9010), settingsGeometry_.milliseconds);
    moveRect(Get(9011), settingsGeometry_.encodingLabel);
    moveRect(Get(ID_ENCODING), settingsGeometry_.encodingCombo);

    const int rightPadding = geometry.cardPadding;
    const int titleTopPadding = settingsGeometry_.serialTitle.top -
                                settingsGeometry_.serialCard.top;
    const int titleHeight = settingsGeometry_.serialTitle.bottom -
                            settingsGeometry_.serialTitle.top;
    const int titleGap = settingsGeometry_.serialFields[0].top -
                         settingsGeometry_.serialTitle.bottom;
    const std::array<int, 5> tools{ID_RX_FILE, ID_PAUSE, ID_COPY_ALL, ID_CLEAR_LOG, ID_EXPORT};
    std::array<int, 5> toolWidths{};
    const int toolGap = scale(6);
    int toolWidth = 4 * toolGap;
    HDC dc = GetDC(window_);
    const auto oldFont = SelectObject(dc, font_);
    for (std::size_t i = 0; i < tools.size(); ++i) {
        const auto text = WindowText(Get(tools[i]));
        SIZE size{}; GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        toolWidths[i] = size.cx + scale(16);
        toolWidth += toolWidths[i];
    }
    SelectObject(dc, titleFont_);
    SIZE titleSize{}; GetTextExtentPoint32W(dc, L"通信记录", 4, &titleSize);
    SelectObject(dc, oldFont); ReleaseDC(window_, dc);
    geometry.commRecordTitle.right = geometry.commRecordTitle.left + titleSize.cx;
    moveRect(Get(9004), geometry.commRecordTitle);
    int toolX = rightX + rightW - rightPadding - toolWidth;
    int toolbarTop = logCard_.top + std::max(4, titleTopPadding / 2);
    const int toolHeight = std::max(32, titleHeight + 8);
    if (toolX < geometry.commRecordTitle.right + toolGap)
        toolbarTop = geometry.commRecordTitle.bottom + toolGap;
    for (std::size_t i = 0; i < tools.size(); ++i) {
        MoveWindow(Get(tools[i]), toolX, toolbarTop, toolWidths[i], toolHeight, TRUE);
        toolX += toolWidths[i] + toolGap;
    }
    const int logTop = std::max<int>(logCard_.top + titleTopPadding + titleHeight + titleGap,
                                toolbarTop + toolHeight + toolGap);
    const int logLeft = rightX + rightPadding;
    const int logRight = rightX + rightW - rightPadding;
    MoveWindow(Get(ID_LOG_VIEW), logLeft, logTop, std::max(1, logRight - logLeft),
               std::max(120, static_cast<int>(logCard_.bottom) - rightPadding - logTop), TRUE);

    moveRect(Get(9005), geometry.dataSendTitle);
    const int actionW = 170;
    const int editTop = sendCard_.top + titleTopPadding + titleHeight + titleGap;
    const int editBottom = sendCard_.bottom - rightPadding;
    const int actionX = rightX + rightW - rightPadding - actionW;
    const int editRight = actionX - gap;
    constexpr int editInset = 4;
    MoveWindow(Get(ID_SEND_EDIT), logLeft + editInset, editTop + editInset,
               std::max(1, editRight - logLeft - editInset * 2),
               std::max(90, editBottom - editTop - editInset * 2), TRUE);
    const std::array<int, 4> actions{ID_SEND, ID_LOAD_FILE, ID_SEND_TIMED, ID_CLEAR_SEND};
    const int actionGap = 6;
    const int actionHeight = std::max(28, (editBottom - editTop - 3 * actionGap) / 4);
    int actionY = editTop;
    for (int id : actions) {
        MoveWindow(Get(id), actionX, actionY, actionW, actionHeight, TRUE);
        actionY += actionHeight + actionGap;
    }
    const auto placeExtension = [&](int id, const RECT& rect, bool visible) {
        HWND control = Get(id);
        if (!visible && GetFocus() == control) SetFocus(Get(ID_SEND_EDIT));
        if (visible) moveRect(control, rect);
        ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
        if (visible && id >= extension::EditFirst &&
            id < extension::EditFirst + kMaximumStoredCustomSlots)
            CenterSingleLineEditText(control);
    };
    placeExtension(extension::CustomTitle, geometry.customDataTitle, !IsRectEmpty(&geometry.customDataTitle));
    placeExtension(extension::ProtocolTitle, geometry.protocolTitle, !IsRectEmpty(&geometry.protocolTitle));
    for (int i = 0; i < kMaximumStoredCustomSlots; ++i) {
        const auto& slot = geometry.customSlots[static_cast<std::size_t>(i)];
        // Rows are laid out at fixed full-column width. The host window clips
        // the portion outside the progressively revealed extension viewport.
        const bool visible = geometry.extensionVisible && i < geometry.visibleCustomRows;
        placeExtension(extension::IndexFirst + i, slot.index, visible);
        placeExtension(extension::EditFirst + i, slot.edit, visible);
        placeExtension(extension::SendFirst + i, slot.send, visible);
    }
    LayoutProtocolControls();
    SetProtocolControlsVisible(geometry.extensionVisible);

    moveRect(Get(ID_STATUS_LEFT), geometry.statusLeft);
    moveRect(Get(ID_STATUS_RIGHT), geometry.statusRight);
    InvalidateRect(window_, nullptr, TRUE);
}

bool Application::Checked(int id) const { return IsDlgButtonChecked(window_, id) == BST_CHECKED; }
int Application::ComboSelection(int id) const { return static_cast<int>(SendMessageW(Get(id), CB_GETCURSEL, 0, 0)); }

void Application::LoadConfiguration() {
    try {
        configStore_ = std::make_unique<config::ConfigStore>(config::ConfigStore::OpenDefault());
        customData_ = configStore_->InitialData();
        loadingConfiguration_ = true;
        for (std::size_t index = 0; index < config::kSlotCount; ++index)
            SetWindowTextW(Get(extension::EditFirst + static_cast<int>(index)),
                           customData_.customData[index].c_str());
        loadingConfiguration_ = false;
    } catch (...) {
        loadingConfiguration_ = false;
        configStore_.reset();
    }
}

void Application::SaveConfiguration() {
    if (configurationSaved_) return;
    configurationSaved_ = true;
    if (!configStore_) return;
    std::wstring error;
    if (configStore_->SaveMerged(customData_, customDataDirty_, error)) return;
    if (logWriter_) logWriter_->Write("[WARNING] Custom data configuration save failed.\r\n");
    MessageBoxW(window_, L"自定义数据配置保存失败。", L"配置保存失败", MB_OK | MB_ICONWARNING);
}

void Application::SetTextEncoding(textcodec::TextEncoding encoding) {
    DrainReceive();
    FlushRxPending();
    encoding_ = encoding;
    const int encodingIndex = encoding_ == textcodec::TextEncoding::Utf8 ? 0 :
                              encoding_ == textcodec::TextEncoding::Gbk ? 1 : 2;
    encodingChoice_->store(encodingIndex, std::memory_order_relaxed);
    rxDecoder_.Reset(encoding_);
    recordView_.SetEncoding(encoding_);
    if (Get(ID_ENCODING)) SendMessageW(Get(ID_ENCODING), CB_SETCURSEL, encodingIndex, 0);
    recordView_.Changed();
}

void Application::FlushRxPending() {
    auto bytes = rxDecoder_.FlushBytes();
    if (!bytes.empty()) {
        records_.Add(comm::Direction::Rx,
                     rxPendingTimestamp_.empty() ? util::Timestamp() : rxPendingTimestamp_,
                     std::move(bytes));
        recordView_.Changed();
    }
    rxPendingTimestamp_.clear();
}

void Application::ScanPorts(bool preserve) {
    HWND combo = Get(ID_PORT);
    std::wstring old = preserve ? WindowText(combo) : std::wstring();
    portInfos_ = EnumerateSerialPorts();
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int selection = -1;
    for (std::size_t i = 0; i < portInfos_.size(); ++i) {
        const auto display = SerialPortDisplayName(portInfos_[i]);
        const LRESULT item = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        SendMessageW(combo, CB_SETITEMDATA, item, static_cast<LPARAM>(i));
        if (_wcsicmp(portInfos_[i].portName.c_str(), old.c_str()) == 0 ||
            _wcsicmp(display.c_str(), old.c_str()) == 0) selection = static_cast<int>(i);
    }
    if (selection < 0 && !portInfos_.empty()) selection = 0;
    SendMessageW(combo, CB_SETCURSEL, selection, 0);
    SendMessageW(combo, CB_SETDROPPEDWIDTH, 500, 0);
}

void Application::OpenPort() {
    if (serial_.IsOpen()) return;
    const int selection = ComboSelection(ID_PORT);
    const SerialPortInfo* selectedInfo = selection >= 0 && static_cast<std::size_t>(selection) < portInfos_.size()
        ? &portInfos_[static_cast<std::size_t>(selection)] : nullptr;
    const std::wstring port = selectedInfo ? selectedInfo->portName : WindowText(Get(ID_PORT));
    if (port.empty()) { MessageBoxW(window_, L"未发现可用串口，请连接设备后重试。", L"串口助手", MB_OK | MB_ICONWARNING); return; }
    SerialConfig config{};
    config.port = port;
    config.baudRate = static_cast<DWORD>(_wtoi(WindowText(Get(ID_BAUD)).c_str()));
    config.dataBits = static_cast<BYTE>(_wtoi(WindowText(Get(ID_DATA_BITS)).c_str()));
    const int stop = ComboSelection(ID_STOP_BITS); config.stopBits = stop == 1 ? ONE5STOPBITS : stop == 2 ? TWOSTOPBITS : ONESTOPBIT;
    const std::array<BYTE, 5> parities{NOPARITY, ODDPARITY, EVENPARITY, MARKPARITY, SPACEPARITY};
    const int parity = std::clamp(ComboSelection(ID_PARITY), 0, 4); config.parity = parities[static_cast<std::size_t>(parity)];
    config.rtsCts = ComboSelection(ID_FLOW) == 1;
    std::wstring error;
    if (!serial_.Open(config,
        [hwnd = window_, ingress = rxIngress_, log = logWriter_, recorder = recorder_, encodingChoice = encodingChoice_](std::vector<std::uint8_t> bytes) {
            if (bytes.empty()) return;
            const std::wstring timestamp = util::Timestamp();
            comm::Record event;
            event.direction = comm::Direction::Rx;
            event.timestamp = timestamp;
            event.rawBytes = bytes;
            const int choice = encodingChoice->load(std::memory_order_relaxed);
            const auto encoding = choice == 1 ? textcodec::TextEncoding::Gbk :
                                  choice == 2 ? textcodec::TextEncoding::Ascii : textcodec::TextEncoding::Utf8;
            log->WriteRecord(event, encoding);
            recorder->Write(RecorderLine(event));
            ingress->Push(timestamp, std::move(bytes));
        },
        [hwnd = window_](DWORD code, std::wstring message) {
            auto* failure = new SerialFailure{code, std::move(message)};
            if (!PostMessageW(hwnd, WM_SERIAL_ERROR, 0, reinterpret_cast<LPARAM>(failure))) delete failure;
        }, error)) {
        AppendSystem(L"打开 " + port + L" 失败：" + error, true);
        MessageBoxW(window_, error.c_str(), L"无法打开串口", MB_OK | MB_ICONERROR);
        return;
    }
    connectedPort_ = port;
    disconnectHandled_ = false;
    SetConnectedUi(true);
    AppendSystem(L"已连接 " + port + (selectedInfo && !selectedInfo->deviceDesc.empty()
        ? L" · " + selectedInfo->deviceDesc : L""));
}

void Application::ClosePort(bool showRecord) {
    const bool wasOpen = serial_.IsOpen();
    disconnectHandled_ = true;
    const auto closeResult = serial_.Close();
    DrainReceive();
    FlushRxPending();
    KillTimer(window_, TIMER_SEND);
    CheckDlgButton(window_, ID_TIMED, BST_UNCHECKED);
    SetConnectedUi(false);
    connectedPort_.clear();
    if (wasOpen && showRecord) AppendSystem(L"串口已关闭");
    if (closeResult == SerialPort::CloseResult::ForcedHandleClose)
        AppendSystem(L"串口驱动未及时响应取消请求，已强制关闭设备句柄。", true);
    else if (closeResult == SerialPort::CloseResult::WorkerDetached)
        AppendSystem(L"串口驱动线程未在超时内退出，已隔离后台状态以保持界面响应。", true);
}

void Application::SetConnectedUi(bool connected) {
    SetWindowTextW(window_, product::kDisplayName);
    EnableWindow(Get(ID_OPEN), !connected);
    EnableWindow(Get(ID_CLOSE), connected);
    for (int id : {ID_PORT, ID_BAUD, ID_DATA_BITS, ID_STOP_BITS, ID_PARITY, ID_FLOW}) EnableWindow(Get(id), !connected);
    if (connected) {
        const int selection = ComboSelection(ID_PORT);
        const std::wstring port = selection >= 0 && static_cast<std::size_t>(selection) < portInfos_.size()
            ? portInfos_[static_cast<std::size_t>(selection)].portName : WindowText(Get(ID_PORT));
        const std::wstring status = L"●  已连接    " + port + L"    " +
            WindowText(Get(ID_BAUD)) + L", " + WindowText(Get(ID_DATA_BITS)) + L", " +
            WindowText(Get(ID_STOP_BITS)) + L", " + WindowText(Get(ID_PARITY)) + L", " +
            WindowText(Get(ID_FLOW));
        SetWindowTextW(Get(ID_STATUS_LEFT), status.c_str());
    } else SetWindowTextW(Get(ID_STATUS_LEFT), L"●  未连接");
    InvalidateRect(Get(ID_STATUS_LEFT), nullptr, TRUE);
}

void Application::StopTimedSendOnError(const std::wstring& reason) {
    if (timedSendBlocked_) return;
    timedSendBlocked_ = true;
    KillTimer(window_, TIMER_SEND);
    CheckDlgButton(window_, ID_TIMED, BST_UNCHECKED);
    const std::wstring detail = reason.empty() ? L"发送失败。" : reason;
    AppendSystem(L"定时发送已停止：" + detail, true);
    MessageBoxW(window_, (L"定时发送已停止：\r\n" + detail).c_str(), L"定时发送", MB_OK | MB_ICONWARNING);
}

bool Application::BuildSendDataFromText(const std::wstring& text, std::vector<std::uint8_t>& data,
                                        bool fromTimer, int customSlot) {
    const std::wstring errorTitle = customSlot >= 0
        ? L"自定义数据 " + std::to_wstring(customSlot + 1) + L" 格式错误"
        : L"发送内容错误";
    if (Checked(ID_TX_HEX)) {
        std::wstring error;
        if (!util::ParseHex(text, data, error)) {
            if (fromTimer) StopTimedSendOnError(error);
            else MessageBoxW(window_, error.c_str(), errorTitle.c_str(), MB_OK | MB_ICONWARNING);
            return false;
        }
    } else {
        std::wstring error;
        if (!textcodec::Encode(text, encoding_, data, error)) {
            if (fromTimer) StopTimedSendOnError(error);
            else MessageBoxW(window_, error.c_str(), errorTitle.c_str(), MB_OK | MB_ICONWARNING);
            return false;
        }
    }
    if (Checked(ID_TX_CR)) data.push_back('\r');
    if (Checked(ID_TX_LF)) data.push_back('\n');
    return !data.empty();
}

void Application::NormalizeHexEditor() {
    if (normalizingHex_ || !Checked(ID_TX_HEX)) return;
    const std::wstring current = WindowText(Get(ID_SEND_EDIT));
    const std::wstring normalized = NormalizeHexEditorText(current);
    if (current == normalized) return;
    normalizingHex_ = true;
    SetWindowTextW(Get(ID_SEND_EDIT), normalized.c_str());
    SendMessageW(Get(ID_SEND_EDIT), EM_SETSEL, normalized.size(), normalized.size());
    normalizingHex_ = false;
}

void Application::NormalizeCustomHexEditor(int slot) {
    if (normalizingCustomHex_ || !Checked(ID_TX_HEX)) return;
    HWND edit = Get(extension::EditFirst + slot);
    const std::wstring current = WindowText(edit);
    const std::wstring normalized = NormalizeHexEditorText(current);
    if (current == normalized) return;
    normalizingCustomHex_ = true;
    SetWindowTextW(edit, normalized.c_str());
    SendMessageW(edit, EM_SETSEL, normalized.size(), normalized.size());
    RedrawWindow(edit, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    normalizingCustomHex_ = false;
}

void Application::ToggleHexEditorMode() {
    HWND edit = Get(ID_SEND_EDIT);
    if (Checked(ID_TX_HEX)) {
        std::wstring error;
        if (!textcodec::Encode(WindowText(edit), encoding_, sendRawSnapshot_, error)) {
            MessageBoxW(window_, error.c_str(), L"编码转换失败", MB_OK | MB_ICONWARNING);
            CheckDlgButton(window_, ID_TX_HEX, BST_UNCHECKED);
            return;
        }
        std::array<std::vector<std::uint8_t>, config::kSlotCount> customBytes;
        for (std::size_t slot = 0; slot < config::kSlotCount; ++slot) {
            if (!textcodec::Encode(WindowText(Get(extension::EditFirst + static_cast<int>(slot))),
                                   encoding_, customBytes[slot], error)) {
                MessageBoxW(window_, error.c_str(),
                            (L"自定义数据 " + std::to_wstring(slot + 1) + L" 编码转换失败").c_str(),
                            MB_OK | MB_ICONWARNING);
                CheckDlgButton(window_, ID_TX_HEX, BST_UNCHECKED);
                return;
            }
        }
        SetWindowTextW(edit, util::FormatBytes(sendRawSnapshot_).c_str());
        SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
        for (std::size_t slot = 0; slot < config::kSlotCount; ++slot)
            SetWindowTextW(Get(extension::EditFirst + static_cast<int>(slot)),
                           util::FormatBytes(customBytes[slot]).c_str());
    } else {
        std::vector<std::uint8_t> parsed;
        std::wstring error;
        if (util::ParseHex(WindowText(edit), parsed, error)) {
            sendRawSnapshot_ = parsed;
            SetWindowTextW(edit, textcodec::Decode(parsed, encoding_).c_str());
        }
        for (std::size_t slot = 0; slot < config::kSlotCount; ++slot) {
            HWND customEdit = Get(extension::EditFirst + static_cast<int>(slot));
            if (util::ParseHex(WindowText(customEdit), parsed, error))
                SetWindowTextW(customEdit, textcodec::Decode(parsed, encoding_).c_str());
        }
    }
}

void Application::SendText(const std::wstring& text, bool fromTimer, int customSlot) {
    if (!fromTimer && Checked(ID_TIMED)) {
        CheckDlgButton(window_, ID_TIMED, BST_UNCHECKED);
        SendMessageW(window_, WM_COMMAND, MAKEWPARAM(ID_TIMED, BN_CLICKED), reinterpret_cast<LPARAM>(Get(ID_TIMED)));
    }
    if (fromTimer && (!Checked(ID_TIMED) || timedSendBlocked_)) return;
    // Sending is an explicit request to return to the live tail when
    // auto-scroll is enabled. Clear the historical-view suppression left by
    // manual scrolling before the next record refresh.
    if (Checked(ID_AUTOLINE)) recordView_.SetAutoFollowEnabled(true);
    if (!serial_.IsOpen()) {
        if (fromTimer) StopTimedSendOnError(L"串口不可用，请重新打开串口。");
        else MessageBoxW(window_, L"请先打开串口。", L"串口助手", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::vector<std::uint8_t> data;
    if (!BuildSendDataFromText(text, data, fromTimer, customSlot)) return;
    if (!serial_.Send(data)) {
        if (fromTimer) StopTimedSendOnError(L"发送队列已满或串口不可用。");
        else MessageBoxW(window_, L"发送队列已满或串口不可用。", L"发送失败", MB_OK | MB_ICONWARNING);
        return;
    }
    txBytes_ += data.size();
    AppendRecord(false, data);
}

void Application::SendData(bool fromTimer) {
    SendText(WindowText(Get(ID_SEND_EDIT)), fromTimer);
}

void Application::AppendSystem(const std::wstring& text, bool error) {
    auto message = text;
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) message.pop_back();
    comm::Record record;
    record.timestamp = util::Timestamp();
    record.direction = error ? comm::Direction::Error : comm::Direction::Notice;
    record.kind = comm::RecordKind::System;
    record.message = message.substr(0, 4096);
    logWriter_->WriteRecord(record, encoding_);
}

void Application::AppendRecord(bool receive, const std::vector<std::uint8_t>& data) {
    if (data.empty()) return;
    const auto& record = records_.Add(receive ? comm::Direction::Rx : comm::Direction::Tx,
                                      util::Timestamp(), data);
    // RX logging is queued before the bounded UI queue. TX is queued here.
    if (!receive) logWriter_->WriteRecord(record, encoding_);
    recorder_->Write(RecorderLine(record));
    recordView_.Changed();
}

void Application::CopyRecords(int mode) {
    const bool selected = mode >= ID_COPY_SELECTED_FULL && mode <= ID_COPY_SELECTED_TEXT;
    const auto format = mode == ID_COPY_HEX || mode == ID_COPY_SELECTED_HEX ? comm::CopyFormat::Hex :
                        mode == ID_COPY_TEXT || mode == ID_COPY_SELECTED_TEXT ? comm::CopyFormat::Text :
                        comm::CopyFormat::Full;
    if (selected && recordView_.SelectedCount() == 0) return;
    if (!PutClipboardText(window_, recordView_.Copy(format, selected)))
        MessageBeep(MB_ICONWARNING);
}

void Application::ShowCopyMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_COPY_FULL, L"复制完整记录");
    AppendMenuW(menu, MF_STRING, ID_COPY_HEX, L"仅复制 HEX");
    AppendMenuW(menu, MF_STRING, ID_COPY_TEXT, L"仅复制文本");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_COPY_SELECTED_FULL, L"复制选中完整记录");
    AppendMenuW(menu, MF_STRING, ID_COPY_SELECTED_HEX, L"复制选中 HEX");
    AppendMenuW(menu, MF_STRING, ID_COPY_SELECTED_TEXT, L"复制选中文本");
    RECT rect{}; GetWindowRect(Get(ID_COPY_ALL), &rect);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN,
                                        rect.left, rect.bottom, 0, window_, nullptr);
    DestroyMenu(menu);
    if (command >= ID_COPY_FULL && command <= ID_COPY_SELECTED_TEXT) CopyRecords(static_cast<int>(command));
}

void Application::DrainReceive() {
    std::deque<RxIngressRecord> batch;
    constexpr std::size_t kMaximumUiBatchBytes = 128 * 1024;
    rxIngress_->Drain(kMaximumUiBatchBytes, batch);
    const std::uint64_t dropped = rxIngress_->TakeDroppedBytes();
    rxBytes_ += rxIngress_->TakeReceivedBytes();
    if (dropped > 0)
        AppendSystem(L"显示队列已满，丢弃最旧显示数据 " + std::to_wstring(dropped) +
                     L" 字节；独立日志队列不受影响。", true);
    if (dropped > 0) FlushRxPending();
    for (auto& item : batch) {
        const bool hadPending = rxDecoder_.PendingSize() != 0;
        if (!hadPending) rxPendingTimestamp_ = item.timestamp;
        auto ready = rxDecoder_.FeedBytes(item.rawBytes);
        if (!ready.empty()) {
            records_.Add(comm::Direction::Rx,
                         rxPendingTimestamp_.empty() ? item.timestamp : rxPendingTimestamp_,
                         std::move(ready));
            recordView_.Changed();
        }
        if (rxDecoder_.PendingSize() != 0) rxPendingTimestamp_ = item.timestamp;
        else rxPendingTimestamp_.clear();
    }
}

void Application::SerialError(std::unique_ptr<SerialFailure> failure) {
    if (!failure || shuttingDown_ || disconnectHandled_) return;
    disconnectHandled_ = true;
    AppendSystem(L"设备已断开或发生通信错误：" + failure->message, true);
    ClosePort(false);
    ScanPorts(true);
    SetWindowTextW(window_, product::kDisplayName);
    MessageBeep(MB_ICONWARNING);
}

void Application::UpdateStatus() {
    const auto logOverflow = logWriter_->TakeOverflowStatus();
    if (logOverflow.records != 0) {
        AppendSystem(L"日志写入速度不足，部分日志已丢失：" +
                     std::to_wstring(logOverflow.bytes) + L" 字节 / " +
                     std::to_wstring(logOverflow.records) + L" 条记录。", true);
        MessageBeep(MB_ICONWARNING);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count();
    wchar_t text[160]{};
    swprintf_s(text, L"RX: %llu    TX: %llu    运行时间: %02lld:%02lld:%02lld", rxBytes_, txBytes_,
               elapsed / 3600, (elapsed / 60) % 60, elapsed % 60);
    SetWindowTextW(Get(ID_STATUS_RIGHT), text);
}

void Application::ToggleLogging() {
    if (logWriter_->IsActive()) {
        logWriter_->Stop();
        SetWindowTextW(Get(ID_LOG), L"启动日志");
        AppendSystem(L"日志记录已停止");
        return;
    }
    const std::wstring existingName = structuredLogPath_.empty() ? L"" : std::filesystem::path(structuredLogPath_).filename().wstring();
    if (structuredLogPath_.empty() || existingName.rfind(L"SerialMate-", 0) == 0) {
        std::wstring stamp = util::Timestamp(true);
        std::replace(stamp.begin(), stamp.end(), L':', L'-');
        structuredLogPath_ = util::ExecutableDirectory() + L"\\SerialMate-" + stamp + L"-" +
                             std::to_wstring(GetCurrentProcessId()) + L".log";
    }
    if (!logWriter_->Start(structuredLogPath_)) {
        MessageBoxW(window_, L"无法创建日志文件，请选择其他路径。", L"日志", MB_OK | MB_ICONERROR); return;
    }
    SetWindowTextW(Get(ID_LOG), L"停止日志");
    AppendSystem(L"日志记录已启动：" + structuredLogPath_);
}

void Application::ToggleRecorder() {
    if (recorder_->IsActive()) {
        recorder_->Stop();
        SetWindowTextW(Get(ID_RX_FILE), L"实时记录");
        return;
    }
    if (!recorder_->IsActive()) {
        const std::wstring path = ChoosePath(window_, true, L"选择实时记录文件", L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0", L"txt");
        if (path.empty()) return;
        if (!recorder_->Start(path)) {
            MessageBoxW(window_, L"无法创建实时记录文件。", L"实时记录", MB_OK | MB_ICONERROR);
            return;
        }
        SetWindowTextW(Get(ID_RX_FILE), L"停止记录");
        return;
    }
}

void Application::ExportLog() {
    const std::wstring path = ChoosePath(window_, true, L"导出通信记录", L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0", L"txt");
    if (path.empty()) return;
    if (!ExportLogPath(path)) MessageBoxW(window_, L"无法写入目标文件。", L"导出失败", MB_OK | MB_ICONERROR);
}

bool Application::ExportLogPath(const std::wstring& path) {
    const std::wstring content = records_.Copy(comm::CopyFormat::Full, std::nullopt, Checked(ID_TIMESTAMP), encoding_);
    const auto bytes = util::Utf8Bytes(content);
    std::ofstream stream(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    static constexpr unsigned char bom[] = {0xef, 0xbb, 0xbf};
    stream.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

bool Application::LoadFilePath(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) return false;
    const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(stream), {});
    SetWindowTextW(Get(ID_SEND_EDIT), textcodec::Decode(bytes, encoding_).c_str());
    return true;
}

void Application::Command(int id, int notification, HWND) {
    if (id == ID_SEND_EDIT && notification == EN_CHANGE) { NormalizeHexEditor(); return; }
    if (id >= extension::EditFirst && id < extension::EditFirst + kMaximumStoredCustomSlots &&
        notification == EN_CHANGE) {
        const std::size_t slot = static_cast<std::size_t>(id - extension::EditFirst);
        NormalizeCustomHexEditor(static_cast<int>(slot));
        customData_.customData[slot] = WindowText(Get(id));
        if (!loadingConfiguration_) customDataDirty_.set(slot);
        return;
    }
    if (id >= extension::SendFirst && id < extension::SendFirst + kMaximumStoredCustomSlots &&
        notification == BN_CLICKED) {
        const int slot = id - extension::SendFirst;
        SendText(WindowText(Get(extension::EditFirst + slot)), false, slot);
        return;
    }
    if (id >= protocolui::FillSlotFirst &&
        id < protocolui::FillSlotFirst + kMaximumStoredCustomSlots) {
        FillProtocolResultIntoCustomSlot(id - protocolui::FillSlotFirst);
        return;
    }
    switch (id) {
    case protocolui::FunctionCombo:
        if (notification == CBN_SELCHANGE) UpdateProtocolForm();
        break;
    case protocolui::Generate: if (notification == BN_CLICKED) GenerateProtocolFrame(); break;
    case protocolui::Copy: if (notification == BN_CLICKED) CopyProtocolResult(); break;
    case protocolui::FillCustom: if (notification == BN_CLICKED) ShowProtocolFillMenu(); break;
    case ID_OPEN: OpenPort(); break;
    case ID_CLOSE: ClosePort(); break;
    case ID_SEND: SendData(); break;
    case ID_TX_HEX: if (notification == BN_CLICKED) ToggleHexEditorMode(); break;
    case ID_CLEAR_SEND: SetWindowTextW(Get(ID_SEND_EDIT), L""); break;
    case ID_CLEAR_LOG:
        recordView_.Clear();
        rxBytes_ = 0;
        txBytes_ = 0;
        UpdateStatus();
        break;
    case ID_COPY_ALL: ShowCopyMenu(); break;
    case ID_COPY_FULL: case ID_COPY_HEX: case ID_COPY_TEXT:
    case ID_COPY_SELECTED_FULL: case ID_COPY_SELECTED_HEX: case ID_COPY_SELECTED_TEXT:
        CopyRecords(id); break;
    case ID_PAUSE: paused_ = !paused_; recordView_.SetPaused(paused_); SetWindowTextW(Get(ID_PAUSE), paused_ ? L"继续显示" : L"暂停显示"); break;
    case ID_EXPORT: ExportLog(); break;
    case ID_TIMESTAMP: if (notification == BN_CLICKED) recordView_.SetTimestamps(Checked(ID_TIMESTAMP)); break;
    case ID_AUTOLINE: if (notification == BN_CLICKED) recordView_.SetAutoFollowEnabled(Checked(ID_AUTOLINE)); break;
    case ID_SIMPLE_MODE: if (notification == BN_CLICKED) recordView_.SetSimpleMode(Checked(ID_SIMPLE_MODE)); break;
    case ID_RX_ONLY: if (notification == BN_CLICKED) recordView_.SetReceiveOnly(Checked(ID_RX_ONLY)); break;
    case ID_RX_HEX: if (notification == BN_CLICKED) recordView_.SetReceiveHex(Checked(ID_RX_HEX)); break;
    case ID_ENCODING:
        if (notification == CBN_SELCHANGE) SetTextEncoding(ComboSelection(ID_ENCODING) == 1
            ? textcodec::TextEncoding::Gbk : ComboSelection(ID_ENCODING) == 2
            ? textcodec::TextEncoding::Ascii : textcodec::TextEncoding::Utf8);
        break;
    case ID_PORT: if (notification == CBN_DROPDOWN && !serial_.IsOpen()) ScanPorts(true); break;
    case ID_LOG: ToggleLogging(); break;
    case ID_RX_FILE: if (notification == BN_CLICKED) ToggleRecorder(); break;
    case ID_TIMED:
        if (Checked(ID_TIMED)) {
            timedSendBlocked_ = false;
            const UINT interval = static_cast<UINT>(std::clamp(_wtoi(WindowText(Get(ID_INTERVAL)).c_str()), 20, 3600000));
            SetTimer(window_, TIMER_SEND, interval, nullptr);
        } else {
            timedSendBlocked_ = false;
            KillTimer(window_, TIMER_SEND);
        }
        SetWindowTextW(Get(ID_SEND_TIMED), Checked(ID_TIMED) ? L"停止定时" : L"定时发送");
        InvalidateRect(Get(ID_SEND_TIMED), nullptr, TRUE);
        break;
    case ID_SEND_TIMED:
        CheckDlgButton(window_, ID_TIMED, Checked(ID_TIMED) ? BST_UNCHECKED : BST_CHECKED);
        SendMessageW(window_, WM_COMMAND, MAKEWPARAM(ID_TIMED, BN_CLICKED), reinterpret_cast<LPARAM>(Get(ID_TIMED)));
        break;
    case ID_LOAD_FILE: {
        const std::wstring path = ChoosePath(window_, false, L"加载文件", L"所有文件 (*.*)\0*.*\0");
        if (!path.empty() && !LoadFilePath(path)) MessageBoxW(window_, L"无法读取所选文件。", L"加载文件", MB_OK | MB_ICONERROR);
        break;
    }
    case ID_NEW: {
        std::wstring modulePath(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(),
                                               static_cast<DWORD>(modulePath.size()));
        if (length != 0 && length < modulePath.size()) {
            modulePath.resize(length);
            ShellExecuteW(window_, L"open", modulePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    }
    case ID_ABOUT: {
        const std::wstring about = std::wstring(product::kDisplayName) + L"  v" + version::kCurrent +
            L"\r\n原生 Windows 串口调试工具\r\n作者：" + product::kAuthor +
            L"\r\n支持 UTF-8 / GBK / ASCII、HEX、定时发送、日志与热拔插恢复。";
        MessageBoxW(window_, about.c_str(), L"关于", MB_OK | MB_ICONINFORMATION);
        break;
    }
    default: break;
    }
}

void Application::Timer(UINT_PTR id) {
    if (id == TIMER_SEND) SendData(true);
    else if (id == TIMER_STATS) UpdateStatus();
    else if (id == TIMER_RECORD_VIEW) {
        // Drain raw receive batches on the UI cadence, then repaint only when
        // the bounded model changed. SerialWorker never formats or paints.
        DrainReceive();
        recordView_.Refresh(Checked(ID_AUTOLINE));
    }
}

void Application::DeviceChanged() {
    if (serial_.IsOpen()) {
        if (!connectedPort_.empty() && !IsSerialPortPresent(connectedPort_)) {
            auto failure = std::make_unique<SerialFailure>();
            failure->code = ERROR_DEVICE_NOT_CONNECTED;
            failure->message = L"当前串口设备已从系统中移除。";
            SerialError(std::move(failure));
        }
        return;
    }
    ScanPorts(true);
}
void Application::UpdateFound(std::unique_ptr<UpdateManifest> manifest) {
    if (!manifest || manifest->version.empty()) return;
    const std::wstring prompt = L"发现新版本 " + manifest->version + L"。是否立即下载并安装？";
    if (MessageBoxW(window_, prompt.c_str(), L"发现新版本", MB_YESNO | MB_ICONINFORMATION) != IDYES) return;
    wchar_t temporaryPath[MAX_PATH]{};
    if (GetTempPathW(static_cast<DWORD>(std::size(temporaryPath)), temporaryPath) == 0) return;
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return;
    wchar_t guidText[40]{};
    if (StringFromGUID2(guid, guidText, static_cast<int>(std::size(guidText))) == 0) return;
    const std::wstring destination = std::wstring(temporaryPath) + L"SerialMate-" +
                                     manifest->version + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                     std::wstring(guidText) + L".download";
    updateChecker_.Download(*manifest, destination, [hwnd = window_](UpdateDownloadResult result) {
        auto* message = new UpdateDownloadResult(std::move(result));
        if (!PostMessageW(hwnd, WM_UPDATE_READY, 0, reinterpret_cast<LPARAM>(message))) delete message;
    });
}

void Application::UpdateReady(std::unique_ptr<UpdateDownloadResult> result) {
    if (!result) return;
    if (!result->success) {
        const std::wstring text = L"更新下载或校验失败：" + result->error;
        MessageBoxW(window_, text.c_str(), L"更新失败", MB_OK | MB_ICONERROR);
        return;
    }
    std::wstring error;
    if (!LaunchUpdateInstaller(*result, error)) {
        MessageBoxW(window_, error.c_str(), L"更新失败", MB_OK | MB_ICONERROR);
        return;
    }
    PostMessageW(window_, WM_CLOSE, 0, 0);
}

void Application::DrawButton(DRAWITEMSTRUCT* draw) {
    const bool primary = GetWindowLongPtrW(draw->hwndItem, GWLP_USERDATA) != 0;
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool timed = GetDlgCtrlID(draw->hwndItem) == ID_SEND_TIMED && Checked(ID_TIMED);
    COLORREF fill = primary ? RGB(0, 115, 232) : (timed ? RGB(225, 244, 232) : RGB(255, 255, 255));
    COLORREF border = primary ? RGB(0, 115, 232) : (timed ? RGB(72, 170, 100) : RGB(205, 216, 231));
    COLORREF text = primary ? RGB(255, 255, 255) : RGB(20, 35, 57);
    if (disabled) { fill = RGB(239, 243, 248); text = RGB(145, 157, 174); border = RGB(220, 227, 236); }
    else if (pressed) fill = MixColor(fill, RGB(0, 0, 0), 10);
    HBRUSH brush = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, border);
    auto oldBrush = SelectObject(draw->hDC, brush); auto oldPen = SelectObject(draw->hDC, pen);
    RoundRect(draw->hDC, draw->rcItem.left, draw->rcItem.top, draw->rcItem.right, draw->rcItem.bottom, 8, 8);
    SelectObject(draw->hDC, oldBrush); SelectObject(draw->hDC, oldPen); DeleteObject(brush); DeleteObject(pen);
    wchar_t label[128]{}; GetWindowTextW(draw->hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(draw->hDC, TRANSPARENT); SetTextColor(draw->hDC, text); SelectObject(draw->hDC, font_);
    RECT textRect = draw->rcItem; DrawTextW(draw->hDC, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

HBRUSH Application::ControlColor(HDC dc, HWND control, UINT message) {
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(19, 34, 57));
    const int id = GetDlgCtrlID(control);
    if (id == ID_STATUS_LEFT && serial_.IsOpen()) SetTextColor(dc, RGB(0, 165, 65));
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) { SetBkColor(dc, RGB(255, 255, 255)); return whiteBrush_; }
    if (id == ID_STATUS_LEFT || id == ID_STATUS_RIGHT) { SetBkColor(dc, RGB(244, 248, 252)); return backgroundBrush_; }
    SetBkColor(dc, RGB(255, 255, 255)); return whiteBrush_;
}

void Application::Paint() {
    const auto& settingsGeometry_ = layout_.settings;
    const auto& logCard_ = layout_.commRecordCard;
    const auto& sendCard_ = layout_.dataSendCard;
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);
    RECT client{};
    GetClientRect(window_, &client);
    FillRect(dc, &client, backgroundBrush_);
    HPEN border = CreatePen(PS_SOLID, 1, RGB(213, 224, 237));
    auto oldPen = SelectObject(dc, border);
    auto oldBrush = SelectObject(dc, whiteBrush_);
    const auto card = [&](int left, int top, int right, int bottom) {
        RoundRect(dc, left, top, right, bottom, 10, 10);
    };
    if (settingsGeometry_.serialCard.right > settingsGeometry_.serialCard.left) {
        card(settingsGeometry_.serialCard.left, settingsGeometry_.serialCard.top,
             settingsGeometry_.serialCard.right, settingsGeometry_.serialCard.bottom);
        card(settingsGeometry_.receiveCard.left, settingsGeometry_.receiveCard.top,
             settingsGeometry_.receiveCard.right, settingsGeometry_.receiveCard.bottom);
        card(settingsGeometry_.sendCard.left, settingsGeometry_.sendCard.top,
             settingsGeometry_.sendCard.right, settingsGeometry_.sendCard.bottom);
    }
    if (logCard_.right > logCard_.left) card(logCard_.left, logCard_.top, logCard_.right, logCard_.bottom);
    if (sendCard_.right > sendCard_.left) card(sendCard_.left, sendCard_.top, sendCard_.right, sendCard_.bottom);
    if (layout_.extensionVisible) {
        const auto& custom = layout_.customDataCard;
        const auto& protocol = layout_.protocolCard;
        card(custom.left, custom.top, custom.right, custom.bottom);
        card(protocol.left, protocol.top, protocol.right, protocol.bottom);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(border);
    EndPaint(window_, &paint);
}

void Application::Shutdown() {
    if (shuttingDown_) return; shuttingDown_ = true;
    SaveConfiguration();
    KillTimer(window_, TIMER_SEND); KillTimer(window_, TIMER_STATS); KillTimer(window_, TIMER_RECORD_VIEW);
    updateChecker_.Stop();
    const auto closeResult = serial_.Close();
    if (closeResult == SerialPort::CloseResult::ForcedHandleClose)
        logWriter_->Write("[WARNING] Serial close required forced handle cancellation.\r\n");
    else if (closeResult == SerialPort::CloseResult::WorkerDetached)
        logWriter_->Write("[WARNING] Serial worker exceeded shutdown timeout and was isolated.\r\n");
    DrainReceive();
    FlushRxPending();
    rxIngress_->Stop();
    recorder_->Stop();
    logWriter_->Stop();
    // All producers are now stopped. Reclaim asynchronous payloads that were
    // posted just before shutdown but will never be dispatched after destroy.
    MSG pending{};
    while (PeekMessageW(&pending, window_, WM_SERIAL_DATA, WM_UPDATE_READY, PM_REMOVE)) {
        if (pending.message == WM_SERIAL_ERROR)
            delete reinterpret_cast<SerialFailure*>(pending.lParam);
        else if (pending.message == WM_UPDATE_FOUND)
            delete reinterpret_cast<UpdateManifest*>(pending.lParam);
        else if (pending.message == WM_UPDATE_READY)
            delete reinterpret_cast<UpdateDownloadResult*>(pending.lParam);
    }
    if (font_) DeleteObject(font_); if (titleFont_) DeleteObject(titleFont_);
    if (whiteBrush_) DeleteObject(whiteBrush_); if (backgroundBrush_) DeleteObject(backgroundBrush_);
    font_ = titleFont_ = nullptr; whiteBrush_ = backgroundBrush_ = nullptr;
}

LRESULT Application::QueryState(WPARAM query) const {
    const auto& settingsGeometry_ = layout_.settings;
    const auto& logCard_ = layout_.commRecordCard;
    const auto& sendCard_ = layout_.dataSendCard;
    switch (query) {
    case 0: return serial_.IsOpen() ? 1 : 0;
    case 1: return static_cast<LRESULT>(std::min<std::uint64_t>(rxBytes_, LONG_MAX));
    case 2: return static_cast<LRESULT>(std::min<std::uint64_t>(txBytes_, LONG_MAX));
    case 3: return static_cast<LRESULT>(records_.RowCount());
    case 4: return paused_ ? 1 : 0;
    case 5: return static_cast<LRESULT>(recordView_.SelectedCount());
    case 6: return static_cast<LRESULT>(recordView_.RowHeight());
    case 7: return static_cast<LRESULT>(recordView_.ContentWidth());
    case 8: return static_cast<LRESULT>(recordView_.HexLeft());
    case 9: return static_cast<LRESULT>(recordView_.BytesPerRow());
    case 10: return encoding_ == textcodec::TextEncoding::Utf8 ? 0 : encoding_ == textcodec::TextEncoding::Gbk ? 1 : 2;
    case 11: return recorder_->IsActive() ? 1 : 0;
    case 20: return settingsGeometry_.serialCard.top;
    case 21: return settingsGeometry_.serialCard.bottom;
    case 22: return settingsGeometry_.receiveCard.top;
    case 23: return settingsGeometry_.receiveCard.bottom;
    case 24: return settingsGeometry_.sendCard.top;
    case 25: return settingsGeometry_.sendCard.bottom;
    case 26: return logCard_.top;
    case 27: return logCard_.bottom;
    case 28: return sendCard_.top;
    case 29: return sendCard_.bottom;
    case 30: return settingsGeometry_.cardGap;
    case 31: return reinterpret_cast<HFONT>(SendMessageW(Get(ID_SEND_EDIT), WM_GETFONT, 0, 0)) == recordView_.Font() ? 1 : 0;
    case 40: return layout_.extensionVisible ? 1 : 0;
    case 41: return layout_.extensionStartWidth;
    case 42: return layout_.extensionFullVisibleWidth;
    case 43: return layout_.extensionColumnWidth;
    case 44: return layout_.visibleCustomRows;
    case 45: return layout_.customDataCard.top;
    case 46: return layout_.customDataCard.bottom;
    case 47: return layout_.protocolCard.top;
    case 48: return layout_.protocolCard.bottom;
    case 49: return static_cast<LRESULT>(layout_.phase);
    case 50: return layout_.defaultClientWidth;
    case 51: return recordView_.MinimumContentWidth8();
    case 52: return layout_.leftColumnWidth;
    case 53: return layout_.mainColumnMinWidth8;
    case 54: return layout_.extensionFullWidth;
    case 55: return layout_.extensionGap;
    case 56: return layout_.extensionContentMinWidth;
    case 57: return GetDpiForWindow(window_);
    case 58: return layout_.settings.serialCard.right - layout_.settings.serialCard.left;
    case 59: return layout_.settings.receiveCard.right - layout_.settings.receiveCard.left;
    case 60: return layout_.settings.sendCard.right - layout_.settings.sendCard.left;
    case 61: return layout_.openButton.left;
    case 62: return layout_.closeButton.right;
    case 63: return layout_.commRecordCard.left;
    case 64: return layout_.commRecordCard.right - layout_.commRecordCard.left;
    case 65: return layout_.currentVisibleContentRight;
    default: return -1;
    }
}

void Application::SetTestValue(ULONG_PTR key, const wchar_t* value) {
    if (!value) return;
    if (key == 1) SetWindowTextW(Get(ID_SEND_EDIT), value);
    else if (key == 2) SetWindowTextW(Get(ID_INTERVAL), value);
    else if (key == 3) LoadFilePath(value);
    else if (key == 4) ExportLogPath(value);
    else if (key == 6) { std::vector<std::uint8_t> bytes(wcslen(value), static_cast<std::uint8_t>('X')); try { records_.Add(comm::Direction::Rx, util::Timestamp(), std::move(bytes)); recordView_.Changed(); } catch (...) {} }
    else if (key == 7) { recordView_.SelectRow(0); CopyRecords(ID_COPY_SELECTED_FULL); }
    else if (key >= 8 && key <= 13) { CopyRecords(static_cast<int>(key - 8 + ID_COPY_FULL)); }
    else if (key == 14) {
        recorder_->Stop();
        if (*value) recorder_->Start(value);
    }
    else if (key >= 100 && key < 100 + config::kSlotCount) {
        const std::size_t slot = static_cast<std::size_t>(key - 100);
        SetWindowTextW(Get(extension::EditFirst + static_cast<int>(slot)), value);
        customData_.customData[slot] = WindowText(Get(extension::EditFirst + static_cast<int>(slot)));
        customDataDirty_.set(slot);
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE:
        app = new Application(window); SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app)); app->Create(); return 0;
    case WM_SIZE: if (app) app->Layout(); return 0;
    case WM_DPICHANGED: {
        if (app) app->Layout();
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (app) app->Layout();
        return 0;
    }
    case WM_PAINT: if (app) { app->Paint(); return 0; } break;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(window);
        // Keep enough room for the left settings panel and an 8-byte
        // HEX/TEXT row, while never making the window larger than the
        // current monitor work area on high-DPI laptop screens.
        RECT minimumRect{0, 0, app ? app->DefaultClientWidth() : 0, 0};
        AdjustWindowRectExForDpi(&minimumRect, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)),
                                 FALSE, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), dpi);
        int minimumWidth = minimumRect.right - minimumRect.left;
        int minimumHeight = MulDiv(680, static_cast<int>(dpi), 96);
        HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        if (GetMonitorInfoW(monitor, &monitorInfo)) {
            const int workWidth = static_cast<int>(monitorInfo.rcWork.right - monitorInfo.rcWork.left);
            const int workHeight = static_cast<int>(monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
            minimumWidth = std::min(minimumWidth, std::max(800, workWidth - 40));
            minimumHeight = std::min(minimumHeight, std::max(600, workHeight - 80));
        }
        info->ptMinTrackSize = {minimumWidth, minimumHeight};
        return 0;
    }
    case WM_COMMAND: if (app) app->Command(LOWORD(wParam), HIWORD(wParam), reinterpret_cast<HWND>(lParam)); return 0;
    case WM_TIMER: if (app) app->Timer(wParam); return 0;
    case WM_DEVICECHANGE: if (app && (wParam == DBT_DEVNODES_CHANGED || wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE)) app->DeviceChanged(); return TRUE;
    case WM_DRAWITEM: if (app) app->DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam)); return TRUE;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        if (app) return reinterpret_cast<LRESULT>(app->ControlColor(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam), message)); break;
    case WM_SERIAL_DATA: {
        if (app) app->DrainReceive();
        return 0;
    }
    case WM_SERIAL_ERROR: {
        // The worker may have posted this immediately before the window was
        // destroyed. Always take ownership of the heap payload, even when
        // there is no longer an Application instance to consume it.
        auto failure = std::unique_ptr<SerialFailure>(reinterpret_cast<SerialFailure*>(lParam));
        if (app) app->SerialError(std::move(failure));
        return 0;
    }
    case WM_UPDATE_FOUND: {
        // Update checks are asynchronous; a result can remain queued after
        // WM_DESTROY. Release it in that case instead of leaking the string.
        auto manifest = std::unique_ptr<UpdateManifest>(reinterpret_cast<UpdateManifest*>(lParam));
        if (app) app->UpdateFound(std::move(manifest));
        return 0;
    }
    case WM_UPDATE_READY: {
        auto result = std::unique_ptr<UpdateDownloadResult>(reinterpret_cast<UpdateDownloadResult*>(lParam));
        if (app) app->UpdateReady(std::move(result));
        return 0;
    }
    case WM_TEST_QUERY: return app ? app->QueryState(wParam) : -1;
    case WM_COPYDATA:
        if (app) {
            const auto* copy = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (copy && copy->lpData && copy->cbData >= sizeof(wchar_t) &&
                (copy->cbData % sizeof(wchar_t)) == 0) {
                const auto* input = static_cast<const wchar_t*>(copy->lpData);
                const std::size_t length = copy->cbData / sizeof(wchar_t);
                const auto terminator = std::find(input, input + length, L'\0');
                if (terminator == input + length) return FALSE;
                const std::wstring value(input, terminator);
                app->SetTestValue(copy->dwData, value.c_str());
                return TRUE;
            }
        }
        return FALSE;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        if (app) { app->Shutdown(); delete app; SetWindowLongPtrW(window, GWLP_USERDATA, 0); }
        PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    int updateExitCode = 0;
    if (TryRunUpdateInstallerMode(updateExitCode)) return updateExitCode;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES}; InitCommonControlsEx(&controls);
    const HMODULE richEditModule = LoadLibraryW(L"Msftedit.dll");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW cls{sizeof(cls)}; cls.style = CS_HREDRAW | CS_VREDRAW; cls.lpfnWndProc = WindowProcedure;
    cls.hInstance = instance; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    const HBRUSH classBrush = CreateSolidBrush(RGB(244, 248, 252));
    cls.hbrBackground = classBrush; cls.lpszClassName = kWindowClass; cls.hIconSm = cls.hIcon;
    if (!RegisterClassExW(&cls)) {
        if (classBrush) DeleteObject(classBrush);
        if (richEditModule) FreeLibrary(richEditModule);
        CoUninitialize();
        return 1;
    }
    HWND window = CreateWindowExW(0, kWindowClass, product::kDisplayName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1536,
                                  1024, nullptr, nullptr, instance, nullptr);
    if (!window) {
        UnregisterClassW(kWindowClass, instance);
        if (classBrush) DeleteObject(classBrush);
        if (richEditModule) FreeLibrary(richEditModule);
        CoUninitialize();
        return 1;
    }
    // Controls now have real font/DPI metrics. Size the hidden initial window
    // to Phase A before showing it, including the native non-client frame.
    const auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    RECT initialClient{0, 0, app->DefaultClientWidth(), 0};
    AdjustWindowRectExForDpi(&initialClient, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)),
                             FALSE, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window));
    RECT initialWindow{}; GetWindowRect(window, &initialWindow);
    SetWindowPos(window, nullptr, 0, 0, initialClient.right - initialClient.left,
                 initialWindow.bottom - initialWindow.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    CenterWindowOnMonitor(window);
    ShowWindow(window, showCommand); UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    UnregisterClassW(kWindowClass, instance);
    if (classBrush) DeleteObject(classBrush);
    if (richEditModule) FreeLibrary(richEditModule);
    CoUninitialize(); return static_cast<int>(message.wParam);
}
