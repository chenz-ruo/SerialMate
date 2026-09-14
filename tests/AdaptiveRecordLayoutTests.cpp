#include "CommView.h"
#include "UiGeometry.h"

#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent) return 2;
    try {
        comm::RecordBuffer buffer;
        std::vector<std::uint8_t> raw(257);
        for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<std::uint8_t>(i);
        buffer.Add(comm::Direction::Rx, L"2026-09-14 06:32:42.059", raw);
        const auto originalCopy = buffer.Copy(comm::CopyFormat::Hex);
        comm::RecordView view(buffer);
        Check(view.Create(parent, 1) != nullptr, "record view creation failed");
        const HWND handle = view.Window();
        for (UINT dpi : {96u, 120u, 144u, 192u}) {
            view.SetDpi(dpi);
            const auto font = view.Font();
            view.SetDpi(dpi);
            Check(view.Font() == font, "unchanged DPI recreated the font");
            const int minContent8 = view.MinimumContentWidth8();
            auto base = CalculateMainLayoutGeometry(1536, 1024, dpi, minContent8);
            base = CalculateMainLayoutGeometry(base.defaultClientWidth, 1024, dpi, minContent8);
            for (int width : {base.defaultClientWidth, base.defaultClientWidth + 80,
                              base.defaultClientWidth + 160, base.extensionFullVisibleWidth,
                              base.extensionFullVisibleWidth + 300,
                              base.extensionFullVisibleWidth + MulDiv(1600, static_cast<int>(dpi), 96)}) {
                const auto layout = CalculateMainLayoutGeometry(width, 1024, dpi, minContent8);
                MoveWindow(view.Window(), 0, 0, layout.rightWidth - 2 * layout.cardPadding, 90, FALSE);
                view.Changed(); view.Refresh(false);
                if (width <= base.extensionFullVisibleWidth)
                    Check(view.BytesPerRow() == 8, "Phase A/B did not naturally retain 8 bytes per row");
                if (width == base.extensionFullVisibleWidth + MulDiv(1600, static_cast<int>(dpi), 96))
                    Check(view.BytesPerRow() == 32, "Phase C failed to reach 32 bytes");
                std::cout << "WidthBudget DPI=" << dpi << " ClientWidth=" << width
                          << " MainMin8=" << layout.mainColumnMinWidth8 << " MainWidth=" << layout.rightWidth
                          << " ExtensionWidth=" << layout.extensionColumnWidth << " BytesPerRow=" << view.BytesPerRow() << '\n';
            }
            view.SetTimestamps(false);
            Check(view.MinimumContentWidth8() == minContent8, "display option changed the base width budget");
            view.SetTimestamps(true);
            for (std::size_t bytes : {8u, 16u, 32u, 16u, 8u}) {
                // Width is derived from measured glyphs, independently of Standard/Wide mode.
                const int width = view.HexLeft() + static_cast<int>(4 * bytes + 1) * view.CharacterWidth() +
                                  MulDiv(8, static_cast<int>(dpi), 96) +
                                  GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(handle)) + 4;
                MoveWindow(view.Window(), 0, 0, width, 90, FALSE);
                view.Changed();
                view.Refresh(false);
                std::cout << "DPI=" << dpi << " ViewWidth=" << width << " BytesPerRow=" << view.BytesPerRow() << '\n';
                Check(view.BytesPerRow() == bytes, "8/16/32 adaptation selected the wrong row width");
                Check(view.Window() == handle, "DPI or resize recreated the record window");
                Check(buffer.Records().front().rawBytes == raw && buffer.ByteCount() == raw.size(),
                      "layout changed raw data");
                Check(buffer.Copy(comm::CopyFormat::Hex) == originalCopy, "layout changed copied raw data");
                std::size_t total = 0;
                for (std::size_t i = 0; i < buffer.RowCount(bytes); ++i) {
                    const auto row = buffer.RowAt(i, bytes);
                    Check(row && row->offset == total && row->count <= bytes, "row bucket offset is incorrect");
                    total += row->count;
                }
                Check(total == raw.size(), "adaptive rows dropped or duplicated bytes");
            }
        }
        DestroyWindow(parent);
        std::cout << "Adaptive8_16_32Tests=Passed\nRawDataPreservationTests=Passed\n";
        return 0;
    } catch (const std::exception& error) {
        DestroyWindow(parent);
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
