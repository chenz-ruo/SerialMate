#include "../src/CommView.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kParentClass[] = L"SerialMateCommViewTestParent";
int failures = 0;
int checks = 0;

LRESULT CALLBACK ParentProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

void Check(bool condition, const wchar_t* name) {
    const int ordinal = checks++;
    if (!condition) {
        std::wcerr << L"FAILED[" << failures << L"] call=" << ordinal << L": " << name << L'\n';
        ++failures;
    }
}

std::vector<std::uint8_t> Bytes(std::size_t count, std::uint8_t seed = 0) {
    std::vector<std::uint8_t> result(count);
    for (std::size_t i = 0; i < count; ++i) result[i] = static_cast<std::uint8_t>(seed + i);
    return result;
}

std::size_t CountText(const std::wstring& text, const std::wstring& needle) {
    if (needle.empty()) return 0;
    std::size_t count = 0;
    for (std::size_t pos = 0; (pos = text.find(needle, pos)) != std::wstring::npos; pos += needle.size()) ++count;
    return count;
}

HWND CreateParent(HINSTANCE instance) {
    WNDCLASSEXW klass{sizeof(klass)};
    klass.lpfnWndProc = ParentProcedure;
    klass.hInstance = instance;
    klass.lpszClassName = kParentClass;
    RegisterClassExW(&klass);
    return CreateWindowExW(0, kParentClass, L"", WS_OVERLAPPEDWINDOW,
                           0, 0, 640, 480, nullptr, nullptr, instance, nullptr);
}

bool PaintToBitmap(HWND view) {
    HDC screen = GetDC(view);
    if (!screen) return false;
    HDC target = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, 640, 240);
    if (!target || !bitmap) {
        if (target) DeleteDC(target);
        if (bitmap) DeleteObject(bitmap);
        ReleaseDC(view, screen);
        return false;
    }
    const HGDIOBJ old = SelectObject(target, bitmap);
    PatBlt(target, 0, 0, 640, 240, WHITENESS);
    SendMessageW(view, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(target), PRF_CLIENT);
    const COLORREF pixel = GetPixel(target, 4, 4);
    SelectObject(target, old);
    DeleteObject(bitmap);
    DeleteDC(target);
    ReleaseDC(view, screen);
    return pixel != CLR_INVALID;
}

int FirstDarkPixelInRow(HWND view, int y0, int y1) {
    HDC screen = GetDC(view);
    if (!screen) return -1;
    HDC target = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, 640, 240);
    if (!target || !bitmap) {
        if (target) DeleteDC(target);
        if (bitmap) DeleteObject(bitmap);
        ReleaseDC(view, screen);
        return -1;
    }
    const HGDIOBJ old = SelectObject(target, bitmap);
    PatBlt(target, 0, 0, 640, 240, WHITENESS);
    SendMessageW(view, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(target), PRF_CLIENT);
    int first = -1;
    for (int x = 0; x < 640 && first < 0; ++x) {
        for (int y = y0; y < y1; ++y) {
            const COLORREF pixel = GetPixel(target, x, y);
            if (pixel != CLR_INVALID && GetRValue(pixel) < 210 &&
                GetGValue(pixel) < 210 && GetBValue(pixel) < 210) {
                first = x;
                break;
            }
        }
    }
    SelectObject(target, old);
    DeleteObject(bitmap);
    DeleteDC(target);
    ReleaseDC(view, screen);
    return first;
}

std::size_t WorkingSetBytes() {
    using Counters = struct {
        DWORD cb;
        DWORD PageFaultCount;
        SIZE_T PeakWorkingSetSize;
        SIZE_T WorkingSetSize;
        SIZE_T QuotaPeakPagedPoolUsage;
        SIZE_T QuotaPagedPoolUsage;
        SIZE_T QuotaPeakNonPagedPoolUsage;
        SIZE_T QuotaNonPagedPoolUsage;
        SIZE_T PagefileUsage;
        SIZE_T PeakPagefileUsage;
    };
    using Query = BOOL(WINAPI*)(HANDLE, Counters*, DWORD);
    HMODULE psapi = LoadLibraryW(L"psapi.dll");
    if (!psapi) return 0;
    const auto query = reinterpret_cast<Query>(GetProcAddress(psapi, "GetProcessMemoryInfo"));
    Counters counters{};
    counters.cb = sizeof(counters);
    const BOOL ok = query && query(GetCurrentProcess(), &counters, sizeof(counters));
    FreeLibrary(psapi);
    return ok ? static_cast<std::size_t>(counters.WorkingSetSize) : 0;
}

void CheckLengthRows() {
    const std::size_t lengths[] = {1, 2, 15, 16, 17, 31, 32, 33, 64, 128, 512, 1024};
    for (const auto length : lengths) {
        comm::RecordBuffer buffer;
        buffer.Add(comm::Direction::Tx, L"T", Bytes(length));
        const std::size_t expected = (length + comm::kBytesPerRow - 1) / comm::kBytesPerRow;
        Check(buffer.RowCount() == expected, L"自适应边界行数");
        for (std::size_t rowIndex = 0; rowIndex < expected; ++rowIndex) {
            const auto row = buffer.RowAt(rowIndex);
            Check(row.has_value() && row->offset == rowIndex * comm::kBytesPerRow,
                  L"VisualRow局部偏移");
            const std::size_t remaining = length - rowIndex * comm::kBytesPerRow;
            Check(row.has_value() && row->count == std::min(comm::kBytesPerRow, remaining),
                  L"VisualRow字节数");
            Check(row.has_value() && row->first == (rowIndex == 0), L"VisualRow首行标记");
        }
        Check(!buffer.RowAt(expected).has_value(), L"VisualRow越界拒绝");
    }
}

void CheckRecordView() {
    comm::RecordBuffer buffer;
    const auto& longRecord = buffer.Add(comm::Direction::Rx, L"2026-09-12 08:25:15.139", Bytes(33, 0x41));
    buffer.Add(comm::Direction::Tx, L"2026-09-12 08:25:16.139", {0x00, 0x09, 0x0a, 0x0d, 0x1b, 0x20, 0x21, 0x7e, 0x7f, 0x80, 0xff});

    HWND parent = CreateParent(GetModuleHandleW(nullptr));
    Check(parent != nullptr, L"view-parent");
    comm::RecordView view(buffer);
    HWND window = view.Create(parent, 7001);
    Check(window != nullptr, L"view-create");
    if (!window) {
        if (parent) DestroyWindow(parent);
        return;
    }
    const auto logicalWidth = [&](int width) {
        return MulDiv(width, static_cast<int>(GetDpiForWindow(window)), 96);
    };
    SetWindowPos(window, nullptr, 0, 0, logicalWidth(900), 220, SWP_NOZORDER | SWP_NOACTIVATE);
    view.Refresh(true);
    UpdateWindow(window);
    Check(view.BytesPerRow() >= 8, L"中等窗口优先保留ASCII列");
    Check(view.PaintedRows() <= view.VisibleRows() + 1, L"view-visible");
    Check(PaintToBitmap(window), L"view-paint");
    const int hexWithTimestamp = view.HexLeft();
    view.SetTimestamps(false);
    Check(view.HexLeft() < hexWithTimestamp, L"关闭时间戳后数据列整体左移");
    Check(view.Copy(comm::CopyFormat::Full, false).find(L"[2026-09-12") == std::wstring::npos,
          L"关闭时间戳后复制不保留时间戳");
    view.SetTimestamps(true);
    SetWindowPos(window, nullptr, 0, 0, logicalWidth(1100), 220, SWP_NOZORDER | SWP_NOACTIVATE);
    Check(view.BytesPerRow() >= 12, L"较宽窗口提升到12字节");
    SetWindowPos(window, nullptr, 0, 0, logicalWidth(1400), 220, SWP_NOZORDER | SWP_NOACTIVATE);
    Check(view.BytesPerRow() == 32, L"超宽窗口提升到32字节");
    SetWindowPos(window, nullptr, 0, 0, logicalWidth(900), 220, SWP_NOZORDER | SWP_NOACTIVATE);

    view.SelectRow(1);
    Check(view.SelectedCount() == 1, L"select-continuation");
    const auto selectedFull = view.Copy(comm::CopyFormat::Full, true);
    Check(selectedFull.find(L"[08:25:15.1]") != std::wstring::npos, L"copy-full-selected");
    Check(CountText(selectedFull, L"[08:25:15.1]") == 1, L"copy-timestamp-once");
    Check(selectedFull.find(L"│ ABCDEFGHIJKLMNOP") != std::wstring::npos, L"copy-hex-ascii");
    Check(view.Copy(comm::CopyFormat::Hex, true).find(L"41 42 43") != std::wstring::npos, L"copy-hex-selected");
    Check(view.Copy(comm::CopyFormat::Text, true).find(L"ABC") != std::wstring::npos, L"copy-text-selected");
    view.SelectRow(longRecord.RowCount(view.BytesPerRow()));
    Check(view.SelectedCount() == 1, L"select-second");
    const auto specialText = view.Copy(comm::CopyFormat::Text, true);
    Check(specialText.find(L"..... !~...") != std::wstring::npos, L"special-ascii");
    Check(view.Copy(comm::CopyFormat::Full, false).find(L"00 09 0A 0D") != std::wstring::npos, L"copy-full-all");
    Check(view.Copy(comm::CopyFormat::Hex, false).find(L"00 09 0A 0D") != std::wstring::npos, L"copy-hex-all");
    Check(view.Copy(comm::CopyFormat::Text, false).find(L"..... !~...") != std::wstring::npos, L"copy-text-all");

    const std::size_t unchangedRows = buffer.RowCount(view.BytesPerRow());
    view.ScrollTo(0);
    const std::size_t oldTop = view.TopRow();
    buffer.Add(comm::Direction::Tx, L"T3", Bytes(32, 0x40));
    view.Changed();
    view.Refresh(false);
    Check(view.TopRow() == oldTop, L"scroll-no-follow");
    const std::size_t addedRows = (32 + view.BytesPerRow() - 1) / view.BytesPerRow();
    Check(buffer.RowCount(view.BytesPerRow()) == unchangedRows + addedRows, L"row-count");
    view.Changed();
    view.Refresh(true);
    const std::size_t expectedBottom = buffer.RowCount(view.BytesPerRow()) > view.VisibleRows()
        ? buffer.RowCount(view.BytesPerRow()) - view.VisibleRows() : 0;
    Check(view.TopRow() == expectedBottom, L"scroll-follow");

    const std::size_t visibleBeforePause = view.VisibleRows();
    view.SetPaused(true);
    const auto frozenCopy = view.Copy(comm::CopyFormat::Full, false);
    Check(view.VisibleRows() + 1 == visibleBeforePause, L"暂停提示占用单行但视图保持有效");
    Check(PaintToBitmap(window), L"暂停提示绘制成功");
    buffer.Add(comm::Direction::Rx, L"LIVE", Bytes(16, 0x60));
    view.Changed();
    view.Refresh(true);
    Check(view.Copy(comm::CopyFormat::Full, false) == frozenCopy, L"pause-freeze");
    view.SetPaused(false);
    view.Refresh(true);
    Check(view.Copy(comm::CopyFormat::Full, false).find(L"[LIVE]") != std::wstring::npos,
          L"resume-live");

    SendMessageW(window, WM_VSCROLL, MAKEWPARAM(SB_TOP, 0), 0);
    buffer.Add(comm::Direction::Rx, L"BROWSE", Bytes(16, 0x30));
    view.Changed();
    view.Refresh(true);
    Check(view.TopRow() == 0, L"手动上滚后新记录不强制拉回底部");
    SendMessageW(window, WM_VSCROLL, MAKEWPARAM(SB_BOTTOM, 0), 0);
    buffer.Add(comm::Direction::Tx, L"FOLLOW", Bytes(16, 0x40));
    view.Changed();
    view.Refresh(true);
    const std::size_t rowsAfterFollow = buffer.RowCount(view.BytesPerRow());
    const std::size_t bottomAfterFollow = rowsAfterFollow > view.VisibleRows()
        ? rowsAfterFollow - view.VisibleRows() : 0;
    Check(view.TopRow() == bottomAfterFollow, L"回到底部后恢复自动跟随");

    const int widths[] = {96, 120, 144};
    int previousCell = 0;
    for (const int dpi : widths) {
        view.SetDpi(static_cast<UINT>(dpi));
        Check(view.CharacterWidth() > 0 && view.RowHeight() > 0 && view.ContentWidth() > 0,
              L"dpi-metrics");
        if (previousCell) Check(view.CharacterWidth() >= previousCell, L"dpi-monotonic");
        previousCell = view.CharacterWidth();
        bool saw8 = false, saw16 = false, saw32 = false, keptTextVisible = true;
        for (int logical = 560; logical <= 1400; logical += 10) {
            SetWindowPos(window, nullptr, 0, 0, MulDiv(logical, dpi, 96), 220,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            saw8 = saw8 || view.BytesPerRow() == 8;
            saw16 = saw16 || view.BytesPerRow() == 16;
            saw32 = saw32 || view.BytesPerRow() == 32;
            RECT adaptiveClient{};
            GetClientRect(window, &adaptiveClient);
            if (view.BytesPerRow() >= 8 && view.ContentWidth() > adaptiveClient.right) {
                keptTextVisible = false;
            }
        }
        Check(saw8 && saw16 && saw32, L"32/16/8字节宽度自适应");
        Check(keptTextVisible, L"自适应尺寸下TEXT始终位于客户区");
    }
    SetWindowPos(window, nullptr, 0, 0, 180, 120, SWP_NOZORDER | SWP_NOACTIVATE);
    view.Changed();
    view.Refresh(false);
    RECT client{}; GetClientRect(window, &client);
    SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS};
    GetScrollInfo(window, SB_HORZ, &horizontal);
    Check(view.ContentWidth() > client.right && horizontal.nMax > static_cast<int>(horizontal.nPage),
          L"horizontal-scroll");
    Check(buffer.RowCount(view.BytesPerRow()) > 0, L"narrow-row-count");

    DestroyWindow(window);
    DestroyWindow(parent);
    (void)longRecord;
}

void CheckSystemRecordAlignment() {
    comm::RecordBuffer buffer;
    buffer.AddMessage(comm::Direction::Notice, L"2026-09-12 11:24:23.341", L"已连接 COM7");
    buffer.AddMessage(comm::Direction::Error, L"2026-09-12 11:25:10.533", L"COM7 已断开");
    HWND parent = CreateParent(GetModuleHandleW(nullptr));
    comm::RecordView view(buffer);
    HWND window = view.Create(parent, 7003);
    Check(window != nullptr, L"system-view-create");
    if (!window) { DestroyWindow(parent); return; }
    SetWindowPos(window, nullptr, 0, 0, 900, 220, SWP_NOZORDER | SWP_NOACTIVATE);
    view.Refresh(true);
    UpdateWindow(window);
    const int first = FirstDarkPixelInRow(window, 4, 40);
    Check(first >= 2 && first < 48, L"system-record-left-aligned");
    const int second = FirstDarkPixelInRow(window, view.RowHeight() + 8, view.RowHeight() * 2 + 8);
    Check(second >= 2 && second < 48, L"system-error-left-aligned");
    Check(view.Copy(comm::CopyFormat::Full, false).find(L"[11:24:23.3]") != std::wstring::npos,
          L"系统消息显示时间戳");
    view.SetTimestamps(false);
    Check(view.Copy(comm::CopyFormat::Full, false).find(L"[11:24:23.3]") == std::wstring::npos,
          L"系统消息时间戳可完全隐藏");
    DestroyWindow(window);
    DestroyWindow(parent);
}

void CheckHighLoad() {
    comm::RecordBuffer buffer;
    HWND parent = CreateParent(GetModuleHandleW(nullptr));
    comm::RecordView view(buffer);
    HWND window = view.Create(parent, 7002);
    Check(window != nullptr, L"创建高负载测试视图");
    if (!window) { DestroyWindow(parent); return; }
    SetWindowPos(window, nullptr, 0, 0, 900, 220, SWP_NOZORDER | SWP_NOACTIVATE);
    const auto started = std::chrono::steady_clock::now();
    auto lastRefresh = started;
    const auto packet = Bytes(1024, 0x30);
    for (int i = 0; i < 10240; ++i) {
        buffer.Add((i & 1) ? comm::Direction::Tx : comm::Direction::Rx,
                   L"stress", packet);
        const auto now = std::chrono::steady_clock::now();
        if (now - lastRefresh >= std::chrono::milliseconds(33)) {
            view.Changed();
            view.Refresh(true);
            UpdateWindow(window);
            lastRefresh = now;
            Check(view.PaintedRows() <= view.VisibleRows() + 1, L"高负载仅绘制可见行");
        }
    }
    view.Changed();
    view.Refresh(true);
    UpdateWindow(window);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    Check(buffer.ByteCount() <= 4 * 1024 * 1024, L"高负载raw缓存有界");
    Check(PaintToBitmap(window), L"高负载WM_PRINTCLIENT绘制成功");
    Check(view.PaintedRows() <= view.VisibleRows() + 1, L"高负载末次绘制有界");
    std::wcout << L"High-load: records=" << buffer.Records().size()
               << L", bytes=" << buffer.ByteCount() << L", rows=" << buffer.RowCount()
               << L", elapsedMs=" << elapsed << L", workingSet=" << WorkingSetBytes() << L"\n";
    DestroyWindow(window);
    DestroyWindow(parent);
}

}

void CheckSimpleContinuity() {
    comm::RecordBuffer buffer;
    buffer.Add(comm::Direction::Tx, L"T1", {0x41});
    buffer.Add(comm::Direction::Rx, L"T2", {0x42});
    HWND parent = CreateParent(GetModuleHandleW(nullptr));
    comm::RecordView view(buffer);
    HWND window = view.Create(parent, 7004);
    Check(window != nullptr, L"简易视图创建");
    if (!window) { DestroyWindow(parent); return; }
    SetWindowPos(window, nullptr, 0, 0, 900, 400, SWP_NOZORDER | SWP_NOACTIVATE);
    const auto rawCopy = view.Copy(comm::CopyFormat::Hex, false);
    view.SetSimpleMode(true);
    view.SetTimestamps(false);
    view.SetReceiveHex(false);
    view.Refresh(true); PaintToBitmap(window);
    Check(view.PaintedRows() == 1, L"无时间戳时相邻收发连续显示");
    buffer.Add(comm::Direction::Rx, L"T3", {0x43});
    view.Changed(); view.Refresh(true); PaintToBitmap(window);
    Check(view.PaintedRows() == 1, L"新增接收数据不另起一行");
    view.SetTimestamps(true);
    view.Refresh(true); PaintToBitmap(window);
    Check(view.PaintedRows() == 6, L"时间戳开启时每条记录独立两行");
    view.SetReceiveOnly(true);
    view.Refresh(true); PaintToBitmap(window);
    Check(view.PaintedRows() == 4, L"过滤发送记录后不保留空行");
    view.SetTimestamps(false);
    view.SetReceiveHex(true);
    view.Refresh(true); PaintToBitmap(window);
    Check(view.PaintedRows() == 1, L"十六进制连续显示不插入记录换行");
    Check(view.Copy(comm::CopyFormat::Hex, false).find(rawCopy) == 0, L"显示变化不修改原始数据");
    DestroyWindow(window); DestroyWindow(parent);
}

void CheckSimpleTextLineEndings() {
    comm::RecordBuffer buffer;
    buffer.Add(comm::Direction::Rx, L"2026-09-14 06:32:42.059",
               {0x41, 0x0d, 0x0a, 0x42, 0x0d, 0x43, 0x0a, 0x44});
    HWND parent = CreateParent(GetModuleHandleW(nullptr));
    comm::RecordView view(buffer);
    HWND window = view.Create(parent, 7006);
    Check(window != nullptr, L"传统文本换行视图创建");
    if (!window) { DestroyWindow(parent); return; }
    SetWindowPos(window, nullptr, 0, 0, 900, 400, SWP_NOZORDER | SWP_NOACTIVATE);
    view.SetSimpleMode(true);
    view.SetReceiveHex(false);
    view.SetTimestamps(true);
    view.Refresh(true);
    Check(view.Copy(comm::CopyFormat::Full, false) == L"[06:32:42.0]←RX\r\nABCD",
          L"传统文本有时间戳时CR LF不显示");
    view.SetTimestamps(false);
    view.Refresh(true);
    Check(view.Copy(comm::CopyFormat::Full, false) == L"A\r\nB\r\nC\r\nD",
          L"传统文本无时间戳时CR LF作为换行");
    DestroyWindow(window); DestroyWindow(parent);
}

void CheckSimpleSelection() {
    comm::RecordBuffer buffer;
    buffer.AddMessage(comm::Direction::Notice, L"", L"ABCDE");
    buffer.AddMessage(comm::Direction::Notice, L"", L"FGHIJ");
    HWND parent = CreateParent(GetModuleHandleW(nullptr));
    comm::RecordView view(buffer);
    HWND window = view.Create(parent, 7005);
    view.SetDpi(96);
    SetWindowPos(window, nullptr, 0, 0, 640, 240, SWP_NOZORDER | SWP_NOACTIVATE);
    view.SetSimpleMode(true);
    view.SetTimestamps(false);
    view.Refresh(false);
    const auto copied = [&]() {
        SendMessageW(window, WM_COPY, 0, 0);
        std::wstring result;
        if (OpenClipboard(window)) {
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            if (data) {
                const auto text = static_cast<const wchar_t*>(GlobalLock(data));
                if (text) { result = text; GlobalUnlock(data); }
            }
            CloseClipboard();
        }
        return result;
    };
    const int cell = view.CharacterWidth();
    const auto start = MAKELPARAM(8 + cell + 1, 9);
    const auto end = MAKELPARAM(8 + 3 * cell + 1, 9 + view.RowHeight());
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, start);
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, end);
    ValidateRect(window, nullptr);
    for (int repeat = 0; repeat < 1000; ++repeat)
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, end);
    Check(!GetUpdateRect(window, nullptr, FALSE), L"传统模式相同字符位置不重复刷新");
    SendMessageW(window, WM_LBUTTONUP, 0, end);
    Check(copied() == L"BCDE\r\nFGH", L"传统模式向下跨行复制完整字符范围");
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, end);
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, start);
    SendMessageW(window, WM_LBUTTONUP, 0, start);
    Check(copied() == L"BCDE\r\nFGH", L"传统模式反向跨行复制相同范围");
    BYTE saved[256]{}, keys[256]{};
    GetKeyboardState(saved);
    keys[VK_CONTROL] = 0x80;
    SetKeyboardState(keys);
    SendMessageW(window, WM_KEYDOWN, 'A', 0);
    SetKeyboardState(saved);
    Check(copied() == L"ABCDE\r\nFGHIJ", L"传统模式Ctrl+A复制全部显示文本");
    Check(PaintToBitmap(window), L"传统模式全选绘制成功");
    DestroyWindow(window); DestroyWindow(parent);
}

int wmain() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CheckLengthRows();
    CheckRecordView();
    CheckSystemRecordAlignment();
    CheckHighLoad();
    CheckSimpleContinuity();
    CheckSimpleTextLineEndings();
    CheckSimpleSelection();
    if (failures == 0) std::wcout << L"All communication view tests passed.\n";
    return failures == 0 ? 0 : 1;
}
