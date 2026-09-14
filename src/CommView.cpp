#include "CommView.h"
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cstring>

namespace comm {
namespace {
constexpr wchar_t kClass[] = L"SerialMateCommView";
std::wstring DisplayTimestamp(const std::wstring& timestamp) {
    const auto separator = timestamp.find(L' ');
    auto value = separator == std::wstring::npos ? timestamp : timestamp.substr(separator + 1);
    const auto dot = value.find(L'.');
    if (dot != std::wstring::npos && dot + 2 < value.size()) value.resize(dot + 2);
    return value;
}
bool Clipboard(HWND owner, const std::wstring& text) {
    if (text.empty()) return false;
    const SIZE_T size = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) return false;
    void* pointer = GlobalLock(memory);
    if (!pointer) { GlobalFree(memory); return false; }
    memcpy(pointer, text.c_str(), size);
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) { GlobalFree(memory); return false; }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(memory);
    return ok;
}
}

RecordView::~RecordView() { if (font_) DeleteObject(font_); }

HWND RecordView::Create(HWND parent, int id) {
    WNDCLASSEXW cls{sizeof(cls)};
    cls.lpfnWndProc = Procedure;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = kClass;
    RegisterClassExW(&cls);
    window_ = CreateWindowExW(0, kClass, L"通信记录：HEX / ASCII",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL,
        0, 0, 100, 100, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), cls.hInstance, this);
    if (window_) SetDpi(GetDpiForWindow(window_));
    return window_;
}

void RecordView::SetDpi(UINT dpi) {
    dpi = dpi ? dpi : 96;
    if (font_ && dpi_ == dpi) return;
    dpi_ = dpi;
    measuredDpi_ = 0;
    if (font_) DeleteObject(font_);
    font_ = CreateFontW(-MulDiv(16, static_cast<int>(dpi_), 96), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    Measure();
    Scrollbars();
    InvalidateRect(window_, nullptr, FALSE);
}

void RecordView::Measure() {
    if (!window_) return;
    HDC dc = GetDC(window_);
    const auto old = SelectObject(dc, font_);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    cell_ = std::max(1L, metrics.tmAveCharWidth);
    pad_ = std::max(2, MulDiv(8, static_cast<int>(dpi_), 96));
    normalRowHeight_ = metrics.tmHeight + std::max(2, MulDiv(4, static_cast<int>(dpi_), 96));
    rowHeight_ = normalRowHeight_;
    simpleLines_.clear(); simpleLastId_ = 0;
    partialSelecting_ = false;
    SIZE timestamp{};
    SIZE direction{};
    if (minimumContentWidth8_ == 0 || measuredDpi_ != dpi_) {
        constexpr wchar_t sample[] = L"[00:00:00.0]";
        GetTextExtentPoint32W(dc, sample, static_cast<int>(std::size(sample) - 1), &timestamp);
        GetTextExtentPoint32W(dc, L"→TX", 3, &direction);
        // Reserve the normal timestamp even when its display is toggled off, so
        // changing display options cannot redistribute the application's columns.
        const int normalHexX = pad_ + timestamp.cx + direction.cx + cell_;
        minimumContentWidth8_ = normalHexX + (8 * 4 + 1) * cell_ + pad_;
        measuredDpi_ = dpi_;
    } else {
        GetTextExtentPoint32W(dc, L"[00:00:00.0]", 12, &timestamp);
        GetTextExtentPoint32W(dc, L"→TX", 3, &direction);
    }
    directionX_ = pad_ + (timestamps_ ? timestamp.cx : 0);
    hexX_ = directionX_ + direction.cx + cell_;
    RECT client{}; GetClientRect(window_, &client);
    bytesPerRow_ = 8;
    for (const std::size_t candidate : {std::size_t(32), std::size_t(16), std::size_t(8)}) {
        const int candidateDivider = hexX_ + static_cast<int>(candidate * 3 - 1) * cell_ + cell_;
        const int candidateAscii = candidateDivider + cell_;
        const int candidateWidth = candidateAscii + static_cast<int>(candidate) * cell_ + pad_;
        if (candidateWidth <= client.right) { bytesPerRow_ = candidate; break; }
    }
    dividerX_ = hexX_ + static_cast<int>(bytesPerRow_ * 3 - 1) * cell_ + cell_;
    asciiX_ = dividerX_ + cell_;
    contentWidth_ = asciiX_ + static_cast<int>(bytesPerRow_) * cell_ + pad_;
    if (simpleMode_) contentWidth_ = client.right;
    if (baseBytesPerRow_ != bytesPerRow_) {
        base_ = Active().BaseRow(bytesPerRow_);
        baseBytesPerRow_ = bytesPerRow_;
    }
    SelectObject(dc, old);
    ReleaseDC(window_, dc);
}

std::size_t RecordView::VisibleRows() const {
    RECT rect{};
    if (window_) GetClientRect(window_, &rect);
    return static_cast<std::size_t>(std::max<int>(1, (rect.bottom - ContentTop() - pad_) / rowHeight_));
}

int RecordView::ContentTop() const { return pad_ + (frozen_ ? rowHeight_ : 0); }

void RecordView::SyncBase() {
    if (simpleMode_) return;
    const std::size_t now = Active().BaseRow(bytesPerRow_);
    if (baseBytesPerRow_ != bytesPerRow_) {
        base_ = now;
        baseBytesPerRow_ = bytesPerRow_;
        return;
    }
    if (now >= base_) top_ = top_ > now - base_ ? top_ - (now - base_) : 0;
    else top_ = 0;
    base_ = now;
}

void RecordView::Scrollbars() {
    if (!window_ || syncing_) return;
    syncing_ = true;
    BuildSimpleLines();
    RECT rect{}; GetClientRect(window_, &rect);
    const bool horizontalOverflow = contentWidth_ > rect.right;
    ShowScrollBar(window_, SB_HORZ, horizontalOverflow);
    GetClientRect(window_, &rect);
    const std::size_t rows = VisibleRowCount();
    const std::size_t page = VisibleRows();
    top_ = std::min(top_, rows > page ? rows - page : 0);
    SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
    vertical.nMax = rows ? static_cast<int>(rows - 1) : 0;
    vertical.nPage = static_cast<UINT>(page);
    vertical.nPos = static_cast<int>(top_);
    SetScrollInfo(window_, SB_VERT, &vertical, TRUE);
    const int viewportWidth = static_cast<int>(rect.right);
    horizontal_ = horizontalOverflow
        ? std::clamp(horizontal_, 0, contentWidth_ - viewportWidth)
        : 0;
    SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS};
    horizontal.nMax = contentWidth_ - 1;
    horizontal.nPage = static_cast<UINT>(std::max<int>(1, rect.right));
    horizontal.nPos = horizontal_;
    SetScrollInfo(window_, SB_HORZ, &horizontal, TRUE);
    syncing_ = false;
}

void RecordView::Refresh(bool follow) {
    if (!dirty_ || frozen_) return;
    BuildSimpleLines();
    SyncBase();
    if (follow && !followSuppressed_) top_ = VisibleRowCount();
    Scrollbars();
    dirty_ = false;
    InvalidateRect(window_, nullptr, FALSE);
}

void RecordView::Clear() {
    simpleLines_.clear(); simpleLastId_ = 0;
    buffer_.Clear();
    if (frozen_) frozen_ = buffer_;
    anchor_ = caret_ = 0;
    top_ = base_ = 0;
    baseBytesPerRow_ = bytesPerRow_;
    horizontal_ = 0;
    followSuppressed_ = false;
    dirty_ = true;
    Scrollbars();
    InvalidateRect(window_, nullptr, FALSE);
}

void RecordView::SetPaused(bool paused) {
    if (paused == frozen_.has_value()) return;
    if (paused) { SyncBase(); frozen_ = buffer_; }
    else frozen_.reset();
    SyncBase();
    dirty_ = true;
    Scrollbars();
    InvalidateRect(window_, nullptr, FALSE);
}

void RecordView::SetTimestamps(bool visible) {
    if (timestamps_ == visible) return;
    timestamps_ = visible;
    Measure(); Scrollbars(); InvalidateRect(window_, nullptr, FALSE);
}

void RecordView::SetAutoFollowEnabled(bool enabled) {
    if (!enabled) return;
    followSuppressed_ = false;
    top_ = VisibleRowCount();
    Scrollbars();
    InvalidateRect(window_, nullptr, FALSE);
}

void RecordView::SetSimpleMode(bool enabled) {
    if (simpleMode_ == enabled) return;
    simpleMode_ = enabled;
    Measure();
    Scrollbars();
    dirty_ = true;
    InvalidateRect(window_, nullptr, FALSE);
}

void RecordView::SetReceiveOnly(bool enabled) {
    if (receiveOnly_ == enabled) return;
    receiveOnly_ = enabled;
    simpleLines_.clear(); simpleLastId_ = 0;
    Scrollbars();
    dirty_ = true;
    InvalidateRect(window_, nullptr, FALSE);
}

void RecordView::SetReceiveHex(bool enabled) {
    if (receiveHex_ == enabled) return;
    receiveHex_ = enabled;
    simpleLines_.clear(); simpleLastId_ = 0;
    Scrollbars();
    dirty_ = true;
    InvalidateRect(window_, nullptr, FALSE);
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> RecordView::Selection() const {
    if (!anchor_ || !caret_) return std::nullopt;
    return std::make_pair(std::min(anchor_, caret_), std::max(anchor_, caret_));
}

std::size_t RecordView::SelectedCount() const {
    const auto range = Selection();
    if (!range) return 0;
    const auto& records = Active().Records();
    return static_cast<std::size_t>(std::count_if(records.begin(), records.end(), [&](const Record& r) {
        return r.id >= range->first && r.id <= range->second;
    }));
}

std::wstring RecordView::Copy(CopyFormat format, bool selected) const {
    if (selected && !Selection()) return {};
    if (simpleMode_ && format == CopyFormat::Full) {
        std::wstring result;
        const auto range = selected ? Selection() : std::nullopt;
        for (const auto& line : simpleLines_) {
            if (range && (line.firstId < range->first || line.firstId > range->second)) continue;
            if (!result.empty()) result += L"\r\n";
            result += line.text;
        }
        return result;
    }
    // While paused, unselected copy must use the frozen snapshot as well;
    // otherwise the clipboard would expose records that are not visible.
    if (format != CopyFormat::Full)
        return Active().Copy(format, selected ? Selection() : std::nullopt, timestamps_, encoding_, bytesPerRow_);
    std::wstring result;
    const auto range = selected ? Selection() : std::nullopt;
    for (const auto& record : Active().Records()) {
        if (range && (record.id < std::min(range->first, range->second) ||
                      record.id > std::max(range->first, range->second))) continue;
        Record displayed = record;
        displayed.timestamp = DisplayTimestamp(record.timestamp);
        auto text = FormatRecord(displayed, format, timestamps_, encoding_, bytesPerRow_);
        if (record.kind == RecordKind::Data) {
            const std::wstring oldPrefix = (timestamps_ ? L"[" + displayed.timestamp + L"]  " : L"") +
                (record.direction == Direction::Rx ? L"← RX  " : L"→ TX  ");
            const std::wstring newPrefix = (timestamps_ ? L"[" + displayed.timestamp + L"]" : L"") +
                (record.direction == Direction::Rx ? L"←RX " : L"→TX ");
            text.replace(0, oldPrefix.size(), newPrefix);
            std::size_t line = text.find(L"\r\n");
            while (line != std::wstring::npos && line + 2 < text.size()) {
                text.replace(line + 2, oldPrefix.size(), newPrefix.size(), L' ');
                line = text.find(L"\r\n", line + 2);
            }
        }
        result += text;
    }
    return result;
}

void RecordView::CopySelection() {
    if (partialSelecting_ && !partialRegionValid_) return;
    if (simpleMode_) {
        if (partialSelecting_) Clipboard(window_, PartialSelectionText());
        return;
    }
    const bool hasPartialRange = partialSelecting_ &&
        (partialStart_ != partialEnd_ || partialAnchorRow_ != partialRow_);
    if (hasPartialRange) {
        Clipboard(window_, PartialSelectionText());
        return;
    }
    Clipboard(window_, Copy(CopyFormat::Full, true));
}

void RecordView::BuildSimpleLines() {
    if (!simpleMode_ || !window_) return;
    const auto& records = Active().Records();
    if (records.empty()) {
        simpleLines_.clear(); simpleFirstId_ = simpleLastId_ = 0;
        return;
    }
    if (simpleFirstId_ != records.front().id || simpleLastId_ > records.back().id) {
        simpleLines_.clear(); simpleLastId_ = 0;
    }
    simpleFirstId_ = records.front().id;
    if (simpleLastId_ == records.back().id) return;
    RECT client{}; GetClientRect(window_, &client);
    const int width = std::max(cell_, static_cast<int>(client.right) - 2 * pad_);
    HDC dc = GetDC(window_);
    const auto old = SelectObject(dc, font_);
    for (const auto& record : records) {
        if (record.id <= simpleLastId_) continue;
        simpleLastId_ = record.id;
        if (receiveOnly_ && record.kind == RecordKind::Data && record.direction == Direction::Tx) continue;
        const bool system = record.kind == RecordKind::System;
        if (system || timestamps_) {
            std::wstring header;
            if (timestamps_) header = L"[" + DisplayTimestamp(record.timestamp) + L"]";
            if (system) {
                if (!header.empty()) header += L"  ";
                header += record.message;
            } else if (timestamps_) {
                header += record.direction == Direction::Rx ? L"←RX" : L"→TX";
            }
            simpleLines_.push_back({std::move(header), record.id, record.id, true,
                                    !system && timestamps_, record.direction});
            if (system) continue;
        }
        std::wstring payload;
        if (receiveHex_) {
            constexpr wchar_t digits[] = L"0123456789ABCDEF";
            for (const auto byte : record.rawBytes) {
                if (!payload.empty()) payload.push_back(L' ');
                payload.push_back(digits[byte >> 4]); payload.push_back(digits[byte & 15]);
            }
            if (!timestamps_ && !simpleLines_.empty() && !simpleLines_.back().header &&
                !simpleLines_.back().text.empty() && !payload.empty()) payload.insert(payload.begin(), L' ');
        } else payload = DisplayText(record.rawBytes, encoding_);
        std::size_t offset = 0;
        while (offset < payload.size()) {
            if (simpleLines_.empty() || simpleLines_.back().header)
                simpleLines_.push_back({L"", record.id, record.id, false, false, record.direction});
            auto& line = simpleLines_.back();
            SIZE occupied{};
            GetTextExtentPoint32W(dc, line.text.data(), static_cast<int>(line.text.size()), &occupied);
            int fit = 0; SIZE extent{};
            GetTextExtentExPointW(dc, payload.data() + offset, static_cast<int>(payload.size() - offset),
                                 std::max(0, width - static_cast<int>(occupied.cx)), &fit, nullptr, &extent);
            if (fit > 0 && offset + fit < payload.size() &&
                payload[offset + fit - 1] >= 0xd800 && payload[offset + fit - 1] <= 0xdbff) --fit;
            if (fit == 0 && !line.text.empty()) {
                simpleLines_.push_back({L"", record.id, record.id, false, false, record.direction});
                continue;
            }
            if (fit == 0) fit = (payload[offset] >= 0xd800 && payload[offset] <= 0xdbff && offset + 1 < payload.size()) ? 2 : 1;
            line.text.append(payload, offset, static_cast<std::size_t>(fit));
            line.lastId = record.id;
            offset += static_cast<std::size_t>(fit);
        }
    }
    SelectObject(dc, old); ReleaseDC(window_, dc);
}

std::size_t RecordView::VisibleRowCount() const {
    if (simpleMode_) return simpleLines_.size();
    if (!receiveOnly_) return Active().RowCount(bytesPerRow_);
    std::size_t count = 0;
    for (std::size_t i = 0, total = Active().RowCount(bytesPerRow_); i < total; ++i) {
        const auto row = Active().RowAt(i, bytesPerRow_);
        if (row && !(row->record->kind == RecordKind::Data && row->record->direction == Direction::Tx)) ++count;
    }
    return count;
}

std::optional<VisualRow> RecordView::VisibleRowAt(std::size_t index) const {
    if (simpleMode_) {
        if (index >= simpleLines_.size()) return std::nullopt;
        const auto id = simpleLines_[index].firstId;
        const auto& records = Active().Records();
        const auto it = std::lower_bound(records.begin(), records.end(), id,
            [](const Record& record, std::uint64_t value) { return record.id < value; });
        if (it == records.end() || it->id != id) return std::nullopt;
        return VisualRow{&*it, 0, it->rawBytes.size(), true};
    }
    if (!receiveOnly_) return Active().RowAt(index, bytesPerRow_);
    for (std::size_t i = 0, total = Active().RowCount(bytesPerRow_); i < total; ++i) {
        const auto row = Active().RowAt(i, bytesPerRow_);
        if (row && !(row->record->kind == RecordKind::Data && row->record->direction == Direction::Tx)) {
            if (index == 0) return row;
            --index;
        }
    }
    return std::nullopt;
}

std::pair<std::size_t, std::size_t> RecordView::SimpleSelectionRange(std::size_t row) const {
    if (!partialSelecting_ || row >= simpleLines_.size()) return {0, 0};
    auto start = std::make_pair(partialAnchorRow_, partialAnchorIndex_);
    auto end = std::make_pair(partialRow_, partialEnd_);
    if (end < start) std::swap(start, end);
    if (row < start.first || row > end.first) return {0, 0};
    const auto length = simpleLines_[row].text.size();
    return {row == start.first ? std::min(start.second, length) : 0,
            row == end.first ? std::min(end.second, length) : length};
}

std::wstring RecordView::PartialSelectionText() const {
    if (!partialSelecting_) return {};
    if (simpleMode_) {
        std::wstring result;
        const auto lo = std::min(partialAnchorRow_, partialRow_);
        const auto hi = std::max(partialAnchorRow_, partialRow_);
        for (auto row = lo; row <= hi && row < simpleLines_.size(); ++row) {
            const auto [first, last] = SimpleSelectionRange(row);
            if (row > lo && (simpleLines_[row].header || simpleLines_[row - 1].header))
                result += L"\r\n";
            result.append(simpleLines_[row].text, first, last - first);
        }
        return result;
    }
    std::wstring text;
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    const auto lo = std::min(partialAnchorRow_, partialRow_);
    const auto hi = std::max(partialAnchorRow_, partialRow_);
    for (std::size_t visual = lo; visual <= hi; ++visual) {
        const auto row = VisibleRowAt(visual);
        if (!row) continue;
        // Normal data selections keep both endpoints inclusive.  Resolve the
        // single-row case explicitly; otherwise both sides would pick the
        // anchor and a leftward drag would collapse to one byte.
        std::size_t first = 0;
        std::size_t last = row->count;
        if (lo == hi) {
            first = std::min(partialAnchorIndex_, partialEnd_);
            last = row->count ? std::min(std::max(partialAnchorIndex_, partialEnd_) + 1, row->count) : 0;
        } else if (visual == lo) {
            first = partialAnchorRow_ == lo ? partialAnchorIndex_ : partialEnd_;
        } else if (visual == hi) {
            const auto lastInclusive = partialAnchorRow_ == hi ? partialAnchorIndex_ : partialEnd_;
            last = row->count ? std::min(lastInclusive + 1, row->count) : 0;
        }
        const auto begin = std::min(first, last);
        const auto end = std::min(std::max(first, last), row->count);
        if (begin >= end) continue;
        if (!text.empty()) text.append(L"\r\n");
        if (!partialHex_) {
            const auto line = DisplayTextRange(row->record->rawBytes, row->offset, row->count, encoding_);
            text.append(line.substr(std::min(begin, line.size()),
                                    std::min(end, line.size()) - std::min(begin, line.size())));
            continue;
        }
        for (std::size_t i = begin; i < end; ++i) {
            if (i != begin) text.push_back(L' ');
            const auto byte = row->record->rawBytes[row->offset + i];
            text.push_back(digits[byte >> 4]); text.push_back(digits[byte & 15]);
        }
    }
    return text;
}

void RecordView::UpdatePartialSelection(int x, int y, bool extend) {
    if (y < ContentTop()) return;
    const std::size_t rowIndex = top_ + static_cast<std::size_t>(std::max(0, y - ContentTop()) / rowHeight_);
    if (simpleMode_) {
        if (rowIndex >= simpleLines_.size()) return;
        const auto& text = simpleLines_[rowIndex].text;
        HDC dc = GetDC(window_); const auto old = SelectObject(dc, font_);
        int fit = 0; SIZE extent{};
        GetTextExtentExPointW(dc, text.data(), static_cast<int>(text.size()),
                             std::max(0, x - pad_), &fit, nullptr, &extent);
        SelectObject(dc, old); ReleaseDC(window_, dc);
        if (fit > 0 && fit < static_cast<int>(text.size()) && text[fit - 1] >= 0xd800 && text[fit - 1] <= 0xdbff) --fit;
        if (extend && partialSelecting_ && rowIndex == partialRow_ &&
            static_cast<std::size_t>(fit) == partialEnd_) return;
        if (!extend || !partialSelecting_) {
            partialStart_ = partialAnchorIndex_ = static_cast<std::size_t>(fit);
            partialAnchorRow_ = rowIndex;
        }
        partialEnd_ = static_cast<std::size_t>(fit); partialRow_ = rowIndex;
        partialSelecting_ = true;
        InvalidateRect(window_, nullptr, FALSE); return;
    }
    const auto row = Active().RowAt(rowIndex, bytesPerRow_);
    if (!row || row->record->kind != RecordKind::Data) return;
    const int contentX = x + horizontal_;
    std::size_t index = 0;
    bool hex = false;
    if (simpleMode_) {
        hex = receiveHex_;
        index = hex ? static_cast<std::size_t>(std::max(0, contentX - pad_) / std::max(1, 3 * cell_))
                    : static_cast<std::size_t>(std::max(0, contentX - pad_) / std::max(1, cell_));
    } else if (contentX >= asciiX_) {
        index = static_cast<std::size_t>(std::max(0, contentX - asciiX_) / std::max(1, cell_));
    } else if (contentX >= hexX_) {
        hex = true;
        index = static_cast<std::size_t>(std::max(0, contentX - hexX_) / std::max(1, 3 * cell_));
    } else {
        partialSelecting_ = false;
        partialRegionValid_ = true;
        SelectRow(rowIndex, true);
        return;
    }
    index = std::min(index, row->count);
    if (!extend || !partialSelecting_) { partialStart_ = index; partialRegionValid_ = true; }
    else partialRegionValid_ = partialHex_ == hex;
    // Keep the current mouse byte as an inclusive endpoint.  Paint/copy turn
    // it into an exclusive bound, so dragging left still includes the byte
    // where the mouse button was pressed.
    partialEnd_ = std::min(index, row->count ? row->count - 1 : 0);
    partialRow_ = rowIndex;
    if (!extend || !partialSelecting_) { partialAnchorRow_ = rowIndex; partialAnchorIndex_ = index; }
    partialHex_ = hex;
    partialSelecting_ = true;
    InvalidateRect(window_, nullptr, FALSE);
}

void RecordView::SelectRow(std::size_t row, bool extend) {
    const auto visual = VisibleRowAt(row);
    if (!visual) return;
    caret_ = visual->record->id;
    if (!extend || !anchor_) anchor_ = caret_;
    InvalidateRect(window_, nullptr, FALSE);
}

void RecordView::ScrollTo(std::size_t row, bool userInitiated) {
    SyncBase(); top_ = row; Scrollbars(); InvalidateRect(window_, nullptr, FALSE);
    if (userInitiated) {
        const std::size_t rows = VisibleRowCount();
        const std::size_t page = VisibleRows();
        const std::size_t bottom = rows > page ? rows - page : 0;
        followSuppressed_ = top_ < bottom;
    }
}

void RecordView::Paint(HDC target) {
    RECT client{}; GetClientRect(window_, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    if (!dc || !bitmap) { if (dc) DeleteDC(dc); if (bitmap) DeleteObject(bitmap); return; }
    auto oldBitmap = SelectObject(dc, bitmap);
    auto oldFont = SelectObject(dc, font_);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    if (frozen_) {
        RECT pausedBar{0, 0, client.right, ContentTop()};
        HBRUSH pausedBrush = CreateSolidBrush(RGB(244, 247, 251));
        FillRect(dc, &pausedBar, pausedBrush);
        DeleteObject(pausedBrush);
        SetTextColor(dc, RGB(95, 114, 142));
        constexpr wchar_t pausedText[] = L"已暂停显示";
        TextOutW(dc, pad_, pad_, pausedText, static_cast<int>(std::size(pausedText) - 1));
    }
    const auto selection = Selection();
    paintedRows_ = 0;
    HPEN dividerPen = CreatePen(PS_SOLID, std::max(1, MulDiv(1, static_cast<int>(dpi_), 96)),
                                RGB(222, 229, 237));
    auto oldPen = SelectObject(dc, dividerPen);
    // Only visible rows are looked up/formatted. CR/LF never enter TextOutW.
    const std::size_t count = VisibleRows() + 1;
    for (std::size_t i = 0; i < count; ++i) {
        if (simpleMode_) {
            if (top_ + i >= simpleLines_.size()) break;
            const auto& line = simpleLines_[top_ + i];
            const auto [first, last] = SimpleSelectionRange(top_ + i);
            if (first < last) {
                SIZE start{}, end{};
                GetTextExtentPoint32W(dc, line.text.data(), static_cast<int>(first), &start);
                GetTextExtentPoint32W(dc, line.text.data(), static_cast<int>(last), &end);
                RECT highlight{pad_ + start.cx, ContentTop() + static_cast<int>(i) * rowHeight_,
                               pad_ + end.cx, ContentTop() + static_cast<int>(i + 1) * rowHeight_};
                HBRUSH brush = CreateSolidBrush(RGB(255, 231, 166));
                FillRect(dc, &highlight, brush); DeleteObject(brush);
            }
            const int lineY = ContentTop() + static_cast<int>(i) * rowHeight_;
            if (line.header && line.hasDirection) {
                const auto marker = line.text.find(line.direction == Direction::Rx ? L"←RX" : L"→TX");
                if (marker != std::wstring::npos) {
                    SetTextColor(dc, RGB(80, 105, 135));
                    TextOutW(dc, pad_, lineY, line.text.data(), static_cast<int>(marker));
                    SIZE prefix{}; GetTextExtentPoint32W(dc, line.text.data(), static_cast<int>(marker), &prefix);
                    SetTextColor(dc, line.direction == Direction::Rx ? RGB(0, 140, 55) : RGB(0, 100, 215));
                    TextOutW(dc, pad_ + prefix.cx, lineY, line.text.data() + marker,
                             static_cast<int>(line.text.size() - marker));
                }
            } else {
                SetTextColor(dc, line.header ? RGB(80, 105, 135) : RGB(18, 31, 53));
                TextOutW(dc, pad_, lineY, line.text.data(), static_cast<int>(line.text.size()));
            }
            ++paintedRows_;
            continue;
        }
        const auto row = VisibleRowAt(top_ + i);
        if (!row) break;
        const Record& record = *row->record;
        const int y = ContentTop() + static_cast<int>(i) * rowHeight_;
        if (receiveOnly_ && record.direction == Direction::Tx) continue;
        RECT line{0, y, client.right, y + rowHeight_};
        if (selection && !partialSelecting_ && record.id >= selection->first && record.id <= selection->second) {
            HBRUSH brush = CreateSolidBrush(RGB(226, 238, 253));
            FillRect(dc, &line, brush); DeleteObject(brush);
        }
        if (partialSelecting_ && partialRegionValid_ && selection &&
            record.id >= selection->first && record.id <= selection->second && record.kind == RecordKind::Data) {
            const auto loRow = std::min(partialAnchorRow_, partialRow_);
            const auto hiRow = std::max(partialAnchorRow_, partialRow_);
            const bool inRange = top_ + i >= loRow && top_ + i <= hiRow;
            std::size_t first = 0;
            std::size_t last = row->count;
            if (inRange && loRow == hiRow) {
                first = std::min(partialAnchorIndex_, partialEnd_);
                last = row->count ? std::min(std::max(partialAnchorIndex_, partialEnd_) + 1, row->count) : 0;
            } else if (inRange && top_ + i == loRow) {
                first = partialAnchorRow_ == loRow ? partialAnchorIndex_ : partialEnd_;
            } else if (inRange && top_ + i == hiRow) {
                const auto lastInclusive = partialAnchorRow_ == hiRow ? partialAnchorIndex_ : partialEnd_;
                last = row->count ? std::min(lastInclusive + 1, row->count) : 0;
            } else if (!inRange) {
                first = last = 0;
            }
            if (first < last) {
                int left = 0;
                int right = 0;
                if (partialHex_) {
                    left = hexX_ + static_cast<int>(first) * 3 * cell_ - horizontal_;
                    right = hexX_ + static_cast<int>(last) * 3 * cell_ - cell_ - horizontal_;
                } else {
                    const auto text = DisplayTextRange(record.rawBytes, row->offset, row->count, encoding_);
                    const auto textFirst = std::min(first, text.size());
                    const auto textLast = std::min(last, text.size());
                    SIZE start{}, end{};
                    GetTextExtentPoint32W(dc, text.data(), static_cast<int>(textFirst), &start);
                    GetTextExtentPoint32W(dc, text.data(), static_cast<int>(textLast), &end);
                    left = asciiX_ + start.cx - horizontal_;
                    right = asciiX_ + end.cx - horizontal_;
                }
                RECT highlight{left, y, right, y + rowHeight_};
                HBRUSH brush = CreateSolidBrush(RGB(255, 231, 166));
                FillRect(dc, &highlight, brush); DeleteObject(brush);
            }
        }
        const auto draw = [&](int x, const wchar_t* text, int n, COLORREF color) {
            SetTextColor(dc, color); TextOutW(dc, x - horizontal_, y, text, n);
        };
        const bool notice = record.kind == RecordKind::System;
        if (notice) {
            // System records are a separate visual type: they do not reserve
            // RX/TX, HEX, or ASCII columns. Render the complete line from the
            // content area's left padding, applying horizontal scroll once.
            std::wstring message;
            if (row->first && timestamps_ && !record.timestamp.empty()) {
                message = L"[" + DisplayTimestamp(record.timestamp) + L"]  ";
            }
            message += record.message.substr(0, 256);
            std::replace(message.begin(), message.end(), L'\r', L' ');
            std::replace(message.begin(), message.end(), L'\n', L' ');
            draw(pad_, message.c_str(), static_cast<int>(message.size()), RGB(80, 105, 135));
        } else {
            MoveToEx(dc, dividerX_ - horizontal_, y, nullptr);
            LineTo(dc, dividerX_ - horizontal_, y + rowHeight_);
            if (row->first && timestamps_ && !record.timestamp.empty()) {
                const auto stamp = L"[" + DisplayTimestamp(record.timestamp) + L"]";
                draw(pad_, stamp.c_str(), static_cast<int>(stamp.size()), RGB(95, 114, 142));
            }
            if (row->first) draw(directionX_, record.direction == Direction::Rx ? L"←RX" : L"→TX", 3,
                                record.direction == Direction::Rx ? RGB(0, 140, 55) : RGB(0, 100, 215));
            constexpr wchar_t digits[] = L"0123456789ABCDEF";
            for (std::size_t j = 0; j < row->count; ++j) {
                const auto byte = record.rawBytes[row->offset + j];
                const wchar_t pair[]{digits[byte >> 4], digits[byte & 15]};
                draw(hexX_ + static_cast<int>(j) * 3 * cell_, pair, 2, RGB(18, 31, 53));
            }
            const std::wstring text = DisplayTextRange(record.rawBytes, row->offset, row->count, encoding_);
            if (!text.empty()) draw(asciiX_, text.c_str(), static_cast<int>(text.size()), RGB(18, 31, 53));
        }
        ++paintedRows_;
    }
    SelectObject(dc, oldPen); DeleteObject(dividerPen);
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(205, 215, 225));
    HGDIOBJ oldBorder = SelectObject(dc, borderPen);
    SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, 0, 0, client.right, client.bottom);
    SelectObject(dc, oldBorder); DeleteObject(borderPen);
    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont); SelectObject(dc, oldBitmap);
    DeleteObject(bitmap); DeleteDC(dc);
}

void RecordView::VerticalScroll(UINT command, int position) {
    std::size_t next = top_;
    const auto page = VisibleRows();
    switch (command) {
    case SB_LINEUP: if (next) --next; break;
    case SB_LINEDOWN: ++next; break;
    case SB_PAGEUP: next = next > page ? next - page : 0; break;
    case SB_PAGEDOWN: next += page; break;
    case SB_TOP: next = 0; break;
    case SB_BOTTOM: next = VisibleRowCount(); break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: next = static_cast<std::size_t>(std::max(0, position)); break;
    default: return;
    }
    ScrollTo(next, true);
}

void RecordView::HorizontalScroll(UINT command, int position) {
    switch (command) {
    case SB_LINELEFT: horizontal_ -= cell_ * 3; break;
    case SB_LINERIGHT: horizontal_ += cell_ * 3; break;
    case SB_PAGELEFT: horizontal_ -= cell_ * 16; break;
    case SB_PAGERIGHT: horizontal_ += cell_ * 16; break;
    case SB_LEFT: horizontal_ = 0; break;
    case SB_RIGHT: horizontal_ = contentWidth_; break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: horizontal_ = position; break;
    default: return;
    }
    Scrollbars(); InvalidateRect(window_, nullptr, FALSE);
}

void RecordView::Keyboard(WPARAM key) {
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const auto& records = Active().Records();
    if (control && key == 'C') { CopySelection(); return; }
    if (control && key == 'A') {
        if (simpleMode_) {
            if (!simpleLines_.empty()) {
                partialSelecting_ = partialRegionValid_ = true;
                partialStart_ = partialAnchorIndex_ = partialAnchorRow_ = 0;
                partialRow_ = simpleLines_.size() - 1;
                partialEnd_ = simpleLines_.back().text.size();
                InvalidateRect(window_, nullptr, FALSE);
            }
            return;
        }
        if (!records.empty()) { anchor_ = records.front().id; caret_ = records.back().id; InvalidateRect(window_, nullptr, FALSE); }
        return;
    }
    if (key == VK_ESCAPE) { partialSelecting_ = false; anchor_ = caret_ = 0; InvalidateRect(window_, nullptr, FALSE); return; }
    if (key == VK_LEFT || key == VK_RIGHT) { HorizontalScroll(key == VK_LEFT ? SB_LINELEFT : SB_LINERIGHT, 0); return; }
    if (records.empty()) return;
    const auto current = std::lower_bound(records.begin(), records.end(), caret_, [](const Record& r, std::uint64_t id) { return r.id < id; });
    std::size_t index = current == records.end() ? records.size() - 1 : static_cast<std::size_t>(current - records.begin());
    if (key == VK_UP) { if (index) --index; }
    else if (key == VK_DOWN) index = std::min(index + 1, records.size() - 1);
    else if (key == VK_HOME) index = 0;
    else if (key == VK_END) index = records.size() - 1;
    else if (key == VK_PRIOR || key == VK_NEXT) {
        VerticalScroll(key == VK_PRIOR ? SB_PAGEUP : SB_PAGEDOWN, 0);
        SelectRow(top_, shift); return;
    } else return;
    if (const auto row = Active().RowOf(records[index].id, bytesPerRow_)) {
        SelectRow(*row, shift);
        if (*row < top_ || *row >= top_ + VisibleRows()) ScrollTo(*row, true);
    }
}

LRESULT CALLBACK RecordView::Procedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* view = reinterpret_cast<RecordView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        view = static_cast<RecordView*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        view->window_ = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
    }
    if (!view) return DefWindowProcW(hwnd, message, wParam, lParam);
    return view->Message(message, wParam, lParam);
}

LRESULT RecordView::Message(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: { PAINTSTRUCT paint{}; HDC dc = BeginPaint(window_, &paint); Paint(dc); EndPaint(window_, &paint); return 0; }
    case WM_PRINTCLIENT: Paint(reinterpret_cast<HDC>(wParam)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: Measure(); Scrollbars(); InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_DPICHANGED_AFTERPARENT: SetDpi(GetDpiForWindow(window_)); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_KEYDOWN: Keyboard(wParam); return 0;
    case WM_COPY: CopySelection(); return 0;
    case WM_LBUTTONDOWN:
        if (GET_Y_LPARAM(lParam) < ContentTop()) return 0;
        SetFocus(window_); dragging_ = true; SetCapture(window_);
        partialSelecting_ = false;
        partialMultiRow_ = false;
        partialRegionValid_ = true;
        mouseDownX_ = GET_X_LPARAM(lParam); mouseDownY_ = GET_Y_LPARAM(lParam);
        if (simpleMode_) {
            followSuppressed_ = true;
            UpdatePartialSelection(mouseDownX_, mouseDownY_, false);
            return 0;
        }
        SelectRow(top_ + static_cast<std::size_t>(std::max(0, GET_Y_LPARAM(lParam) - ContentTop()) / rowHeight_), (wParam & MK_SHIFT) != 0);
        return 0;
    case WM_MOUSEMOVE:
        if (dragging_) {
            const std::size_t row = top_ + static_cast<std::size_t>(std::max(0, GET_Y_LPARAM(lParam) - ContentTop()) / rowHeight_);
            if (!partialSelecting_ && (std::abs(GET_X_LPARAM(lParam) - mouseDownX_) > 3 ||
                                       std::abs(GET_Y_LPARAM(lParam) - mouseDownY_) > 3)) {
                UpdatePartialSelection(mouseDownX_, mouseDownY_, false);
            }
            if (!simpleMode_) SelectRow(row, true);
            if (partialSelecting_) UpdatePartialSelection(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), true);
        }
        return 0;
    case WM_LBUTTONUP: dragging_ = false; if (GetCapture() == window_) ReleaseCapture(); return 0;
    case WM_RBUTTONUP: {
        HMENU menu = CreatePopupMenu();
        if (!menu) return 0;
        AppendMenuW(menu, MF_STRING, 1, L"复制");
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(window_, &point);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                             point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (command == 1) CopySelection();
        return 0;
    }
    case WM_CAPTURECHANGED: dragging_ = false; return 0;
    case WM_MOUSEWHEEL: {
        wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(wParam);
        const int lines = wheelRemainder_ / WHEEL_DELTA * 3;
        wheelRemainder_ %= WHEEL_DELTA;
        ScrollTo(static_cast<std::size_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(top_) - lines)), true); return 0;
    }
    case WM_VSCROLL: case WM_HSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_TRACKPOS};
        GetScrollInfo(window_, message == WM_VSCROLL ? SB_VERT : SB_HORZ, &info);
        if (message == WM_VSCROLL) VerticalScroll(LOWORD(wParam), info.nTrackPos);
        else HorizontalScroll(LOWORD(wParam), info.nTrackPos);
        return 0;
    }
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}
}
