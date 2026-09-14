#pragma once

#include <windows.h>
#include "CommRecord.h"

namespace comm {

// A single native viewport. No child windows or formatted history per row.
class RecordView final {
public:
    explicit RecordView(RecordBuffer& buffer) : buffer_(buffer) {}
    ~RecordView();
    HWND Create(HWND parent, int id);
    HWND Window() const { return window_; }
    void Changed() { dirty_ = true; }
    void Refresh(bool follow);
    void Clear();
    void SetPaused(bool paused);
    void SetTimestamps(bool visible);
    void SetEncoding(textcodec::TextEncoding encoding) { encoding_ = encoding; simpleLines_.clear(); simpleLastId_ = 0; Scrollbars(); dirty_ = true; InvalidateRect(window_, nullptr, TRUE); }
    void SetAutoFollowEnabled(bool enabled);
    void SetSimpleMode(bool enabled);
    void SetReceiveOnly(bool enabled);
    void SetReceiveHex(bool enabled);
    std::wstring Copy(CopyFormat format, bool selected) const;
    void SelectRow(std::size_t row, bool extend = false);
    void ScrollTo(std::size_t row, bool userInitiated = false);
    std::size_t TopRow() const { return top_; }
    std::size_t VisibleRows() const;
    std::size_t SelectedCount() const;
    std::size_t PaintedRows() const { return paintedRows_; }
    void SetDpi(UINT dpi);
    HFONT Font() const { return font_; }
    int RowHeight() const { return rowHeight_; }
    int ContentWidth() const { return contentWidth_; }
    int MinimumContentWidth8() const { return minimumContentWidth8_; }
    int HexLeft() const { return hexX_; }
    int AsciiLeft() const { return asciiX_; }
    int CharacterWidth() const { return cell_; }
    std::size_t BytesPerRow() const { return bytesPerRow_; }
private:
    static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT Message(UINT, WPARAM, LPARAM);
    const RecordBuffer& Active() const { return frozen_ ? *frozen_ : buffer_; }
    std::optional<std::pair<std::uint64_t, std::uint64_t>> Selection() const;
    void Measure();
    void Scrollbars();
    void Paint(HDC dc);
    void VerticalScroll(UINT command, int position);
    void HorizontalScroll(UINT command, int position);
    void Keyboard(WPARAM key);
    void CopySelection();
    std::wstring PartialSelectionText() const;
    std::pair<std::size_t, std::size_t> SimpleSelectionRange(std::size_t row) const;
    void UpdatePartialSelection(int x, int y, bool extend);
    void SyncBase();
    std::size_t VisibleRowCount() const;
    std::optional<VisualRow> VisibleRowAt(std::size_t index) const;
    int ContentTop() const;
    void BuildSimpleLines();
    struct SimpleLine {
        std::wstring text;
        std::uint64_t firstId = 0, lastId = 0;
        bool header = false;
        bool hasDirection = false;
        Direction direction = Direction::Notice;
    };
    std::vector<SimpleLine> simpleLines_;
    std::uint64_t simpleFirstId_ = 0, simpleLastId_ = 0;

    RecordBuffer& buffer_;
    std::optional<RecordBuffer> frozen_;
    HWND window_{};
    HFONT font_{};
    UINT dpi_ = 96;
    int cell_ = 8, rowHeight_ = 20, pad_ = 8;
    int normalRowHeight_ = 20;
    int directionX_ = 0, hexX_ = 0, dividerX_ = 0, asciiX_ = 0, contentWidth_ = 0;
    int minimumContentWidth8_ = 0;
    unsigned measuredDpi_ = 0;
    int horizontal_ = 0, wheelRemainder_ = 0;
    std::size_t top_ = 0, base_ = 0, paintedRows_ = 0;
    std::size_t baseBytesPerRow_ = kBytesPerRow;
    std::size_t bytesPerRow_ = kBytesPerRow;
    std::uint64_t anchor_ = 0, caret_ = 0;
    bool dirty_ = true, timestamps_ = true, syncing_ = false, dragging_ = false;
    bool followSuppressed_ = false;
    bool simpleMode_ = false, receiveOnly_ = false, receiveHex_ = true;
    bool partialSelecting_ = false;
    bool partialMultiRow_ = false;
    bool partialRegionValid_ = true;
    int mouseDownX_ = 0, mouseDownY_ = 0;
    std::size_t partialRow_ = 0, partialStart_ = 0, partialEnd_ = 0;
    std::size_t partialAnchorRow_ = 0, partialAnchorIndex_ = 0;
    bool partialHex_ = false;
    textcodec::TextEncoding encoding_ = textcodec::TextEncoding::Utf8;
};
}
