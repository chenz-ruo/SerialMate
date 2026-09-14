#include "UiGeometry.h"

#include <array>
#include <iostream>

namespace {
int failures = 0;

void Check(bool condition, const char* name) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << name << '\n';
    }
}

bool Contains(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left && inner.top >= outer.top &&
           inner.right <= outer.right && inner.bottom <= outer.bottom;
}

bool Empty(const RECT& rect) {
    return rect.left == 0 && rect.top == 0 && rect.right == 0 && rect.bottom == 0;
}

int Width(const RECT& rect) { return rect.right - rect.left; }
int Height(const RECT& rect) { return rect.bottom - rect.top; }

void CheckExtensions(const MainLayoutGeometry& layout) {
    if (!layout.extensionVisible) {
        Check(Empty(layout.extensionColumn) && Empty(layout.customDataCard) && Empty(layout.protocolCard),
              "Standard mode has no extension rectangles");
        Check(Empty(layout.customDataTitle) && Empty(layout.protocolTitle), "Standard has no orphan titles");
        Check(layout.visibleCustomRows == 0 && layout.extensionColumnWidth == 0,
              "Standard extensions consume no space");
        for (const auto& slot : layout.customSlots)
            Check(Empty(slot.row) && Empty(slot.index) && Empty(slot.edit) && Empty(slot.send),
                  "hidden slots have empty layout rectangles");
        return;
    }
    Check(layout.customDataCard.top == layout.commRecordCard.top &&
          layout.customDataCard.bottom == layout.commRecordCard.bottom,
          "custom card shares both communication boundaries");
    Check(layout.protocolCard.top == layout.dataSendCard.top &&
          layout.protocolCard.bottom == layout.dataSendCard.bottom,
          "protocol card shares both send boundaries");
    Check(layout.customDataCard.bottom < layout.protocolCard.top &&
          layout.protocolCard.top - layout.customDataCard.bottom == layout.mainCardGap,
          "both columns share exactly the same card gap");
    Check(Contains(layout.extensionFullColumn, layout.customDataCard) &&
          Contains(layout.extensionFullColumn, layout.protocolCard) &&
          layout.extensionColumnWidth <= layout.extensionFullWidth,
          "extension cards use fixed design width and visible viewport");
    if (!Empty(layout.customDataTitle)) {
        Check(Contains(layout.customDataCard, layout.customDataTitle) &&
              Contains(layout.protocolCard, layout.protocolTitle), "extension titles stay inside cards");
        Check(layout.customDataTitle.top == layout.commRecordTitle.top &&
              layout.protocolTitle.top == layout.dataSendTitle.top, "matching title baselines");
    }
    Check(layout.visibleCustomRows >= 0 && layout.visibleCustomRows <= kMaximumStoredCustomSlots,
          "visible slot count is bounded");
    Check(layout.visibleCustomRows > 0, "any revealed viewport keeps rows available for clipping");
    for (int i = 0; i < kMaximumStoredCustomSlots; ++i) {
        const auto& slot = layout.customSlots[static_cast<std::size_t>(i)];
        if (i >= layout.visibleCustomRows) {
            Check(Empty(slot.row) && Empty(slot.index) && Empty(slot.edit) && Empty(slot.send),
                  "invisible slot does not participate in geometry");
            continue;
        }
        Check(slot.index.top == slot.edit.top && slot.edit.top == slot.send.top &&
              slot.send.top == slot.row.top, "index/edit/send share one row top");
        Check(slot.index.bottom == slot.row.bottom && slot.edit.bottom == slot.row.bottom &&
              slot.send.bottom == slot.row.bottom, "index/edit/send share one row height");
        Check(Height(slot.row) == Height(layout.settings.interval) &&
              Height(slot.edit) == Height(layout.settings.interval) &&
              Height(slot.send) == Height(layout.settings.interval),
              "custom edit and send button match the interval edit height");
        Check(Contains(layout.customDataCard, slot.row) && Contains(slot.row, slot.index) &&
              Contains(slot.row, slot.edit) && Contains(slot.row, slot.send), "entire slot is inside custom card");
        Check(slot.index.right < slot.edit.left && slot.edit.right < slot.send.left && Width(slot.edit) > 0,
              "slot controls cannot overlap or swap columns");
        if (i) Check(slot.row.top > layout.customSlots[static_cast<std::size_t>(i - 1)].row.bottom,
                     "successive slots have a gap");
    }
}

void PrintRect(const char* name, const RECT& rect) {
    std::cout << name << "=[" << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom << "]\n";
}

void Report(int width, int height, unsigned dpi, const MainLayoutGeometry& layout) {
    std::cout << "MetricSource=Synthetic ClientWidth=" << width << " ClientHeight=" << height << " DPI=" << dpi
              << " WidthPhase=" << static_cast<int>(layout.phase)
              << " LeftColumnWidth=" << layout.leftColumnWidth
              << " MainWidth=" << layout.rightWidth
              << " MainMin8=" << layout.mainColumnMinWidth8
              << " DefaultClientWidth=" << layout.defaultClientWidth
              << " ExtensionGap=" << layout.extensionGap
              << " ExtensionColumnWidth=" << layout.extensionColumnWidth
              << " VisibleCustomRows=" << layout.visibleCustomRows << '\n';
    PrintRect("commRecordCard", layout.commRecordCard);
    PrintRect("dataSendCard", layout.dataSendCard);
    PrintRect("customDataCard", layout.customDataCard);
    PrintRect("protocolCard", layout.protocolCard);
}

void CheckResponsiveLayout() {
    for (unsigned dpi : {96u, 120u, 144u, 192u}) {
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        // Synthetic measured content width; real font measurements are exercised
        // by AdaptiveRecordLayoutTests and the native EXE integration test.
        const int content8 = scale(450);
        const auto initial = CalculateMainLayoutGeometry(1536, 1024, dpi, content8);
        const int base = initial.defaultClientWidth;
        const int full = initial.extensionFullVisibleWidth;
        const auto w0 = CalculateMainLayoutGeometry(base, 1024, dpi, content8);
        Check(w0.rightWidth == w0.mainColumnMinWidth8 && w0.extensionColumnWidth == 0 &&
              w0.phase == WidthPhase::Base, "W0 is the measured 8-byte base width");
        Check(w0.extensionStartWidth == base + 1, "extension begins immediately after the default width");
        Check(w0.leftColumnWidth == scale(kLeftColumnWidthLogical) &&
              kLeftColumnWidthLogical == 219 &&
              w0.extensionFullWidth == scale(kExtensionFullWidthLogical),
              "both side widths are fixed logical dimensions");
        for (const auto& card : {w0.settings.serialCard, w0.settings.receiveCard, w0.settings.sendCard})
            Check(card.right - card.left == w0.leftColumnWidth && card.left == w0.serialColumn.left &&
                  card.right == w0.serialColumn.right, "settings cards use the fixed left width");
        auto previous = w0;
        for (int width = base; width <= full + 600; ++width) {
            const auto layout = CalculateMainLayoutGeometry(width, 1024, dpi, content8);
            CheckExtensions(layout);
            for (const auto& card : {layout.settings.serialCard, layout.settings.receiveCard,
                                     layout.settings.sendCard})
                Check(card.right - card.left == layout.leftColumnWidth &&
                      card.left == layout.serialColumn.left && card.right == layout.serialColumn.right,
                      "settings Card width remains fixed at every reveal state");
            Check(layout.leftColumnWidth == w0.leftColumnWidth &&
                  layout.extensionFullWidth == w0.extensionFullWidth, "side width budgets never grow");
            Check(layout.extensionColumnWidth <= layout.extensionFullWidth, "extension never exceeds full width");
            const int budgetRight = layout.rightX + layout.rightWidth + layout.extensionGap +
                                    layout.extensionColumnWidth + scale(20);
            const int expectedBudgetRight = width == base ? width : std::max(width, base + w0.horizontalGap);
            Check(budgetRight == expectedBudgetRight, "width budget preserves fixed gap reservation");
            Check(layout.extensionColumnWidth >= previous.extensionColumnWidth &&
                  layout.extensionColumnWidth - previous.extensionColumnWidth <= 1 &&
                  layout.rightWidth >= previous.rightWidth && layout.rightWidth - previous.rightWidth <= 1,
                  "a one-pixel drag cannot cause a column jump");
            if (width <= full) {
                Check(layout.rightWidth == w0.mainColumnMinWidth8, "Phase B allocates nothing extra to main");
                Check(layout.extensionColumnWidth == std::clamp(width - base - w0.horizontalGap,
                                                                  0, w0.extensionFullWidth),
                      "Phase B reveals only width beyond the fixed gap");
                if (layout.extensionVisible)
                    Check(layout.extensionGap == w0.horizontalGap,
                          "visible extension keeps a fixed horizontal gap");
                Check((layout.extensionColumnWidth == layout.extensionFullWidth) == (width == full),
                      "card and gap finish expanding at the same pixel");
            } else {
                Check(layout.extensionColumnWidth == w0.extensionFullWidth &&
                      layout.extensionGap == w0.horizontalGap &&
                      layout.rightWidth == w0.mainColumnMinWidth8 + width - full,
                      "Phase C sends all additional width to the main column");
            }
            previous = layout;
        }
        for (int width : {base, base + 80, base + 100, base + 160, full - 1, full, full + 1,
                          full + 100, full + 200, full + 300, full + 600})
            Report(width, 1024, dpi, CalculateMainLayoutGeometry(width, 1024, dpi, content8));
        for (int visible : {40, 100, 200, w0.extensionFullWidth}) {
            const int width = base + w0.horizontalGap + visible;
            const auto layout = CalculateMainLayoutGeometry(width, 1024, dpi, content8);
            if (visible > 0) {
                Check(layout.extensionGap == w0.horizontalGap,
                      "all progressive reveal states keep the fixed horizontal gap");
                Check(layout.customDataCard.left - layout.commRecordCard.right == w0.horizontalGap &&
                      layout.protocolCard.left - layout.dataSendCard.right == w0.horizontalGap,
                      "extension card left edge is main right plus fixed gap");
            }
            Check(layout.dataSendCard.top - layout.commRecordCard.bottom == layout.mainCardGap &&
                  layout.protocolCard.top - layout.customDataCard.bottom == layout.mainCardGap,
                  "vertical card gaps remain fixed during reveal");
        }
        for (int height : {680, 768, 900, 1024, 1080, 1280, 1600}) {
            const auto layout = CalculateMainLayoutGeometry(full, height, dpi, content8);
            CheckExtensions(layout);
            if (layout.visibleCustomRows > 0 && layout.visibleCustomRows < kMaximumStoredCustomSlots) {
                const auto& last = layout.customSlots[static_cast<std::size_t>(layout.visibleCustomRows - 1)];
                const int step = Height(layout.settings.interval) + MulDiv(4, static_cast<int>(dpi), 96);
                Check(last.row.bottom + step > layout.customDataCard.bottom - MulDiv(12, static_cast<int>(dpi), 96),
                      "another complete row cannot fit in the unused space");
            }
        }
        if (dpi == 144) {
            const auto layout = CalculateMainLayoutGeometry(full, 1024, dpi, content8);
            Check(layout.visibleCustomRows == 12,
                  "144 DPI at 1024px height shows twelve complete custom rows");
        }
    }
}
}  // namespace

int main() {
    struct Case { int width; int height; unsigned dpi; };
    const std::array<Case, 12> cases{{
        {1536, 1024, 96}, {1920, 1080, 96}, {1366, 768, 96}, {1280, 720, 96},
        {1536, 1024, 120}, {1920, 1080, 120}, {1366, 768, 120}, {1280, 720, 120},
        {1536, 1024, 144}, {1920, 1080, 144}, {1366, 768, 144}, {1280, 720, 144},
    }};
    for (const auto& item : cases) {
        const int leftWidth = MulDiv(315, static_cast<int>(item.dpi), 96);
        const auto layout = CalculateSettingsGeometry(20, 82, leftWidth, item.height, item.dpi);
        Check(layout.serialCard.bottom < layout.receiveCard.top &&
                  layout.receiveCard.bottom < layout.sendCard.top,
              "cards do not overlap");
        Check(layout.receiveCard.top - layout.serialCard.bottom == layout.cardGap &&
                  layout.sendCard.top - layout.receiveCard.bottom == layout.cardGap,
              "card gaps are equal");
        Check(Contains(layout.serialCard, layout.serialTitle) &&
                  Contains(layout.receiveCard, layout.receiveTitle) &&
                  Contains(layout.sendCard, layout.sendTitle),
              "section titles remain inside cards");
        for (std::size_t index = 0; index < layout.serialFields.size(); ++index) {
            Check(Contains(layout.serialCard, layout.serialLabels[index]) &&
                      Contains(layout.serialCard, layout.serialFields[index]),
                  "serial controls remain inside card");
        }
        Check(Contains(layout.receiveCard, layout.timestamp) &&
                  Contains(layout.receiveCard, layout.autoScroll) &&
                  Contains(layout.receiveCard, layout.simpleMode) &&
                  Contains(layout.receiveCard, layout.receiveOnly) &&
                  Contains(layout.receiveCard, layout.receiveHex),
              "receive controls remain inside card");
        Check(Contains(layout.sendCard, layout.txHex) && Contains(layout.sendCard, layout.txCr) &&
                  Contains(layout.sendCard, layout.txLf) && Contains(layout.sendCard, layout.timed) &&
                  Contains(layout.sendCard, layout.interval) &&
                  Contains(layout.sendCard, layout.milliseconds) &&
                  Contains(layout.sendCard, layout.encodingLabel) &&
                  Contains(layout.sendCard, layout.encodingCombo),
              "send controls remain inside card");
        const int receiveBottomPadding = layout.receiveCard.bottom - layout.simpleMode.bottom;
        const int sendBottomPadding = layout.sendCard.bottom - layout.encodingCombo.bottom;
        Check(receiveBottomPadding >= 8 && receiveBottomPadding <= 24,
              "receive card bottom padding is compact");
        Check(sendBottomPadding >= 0,
              "send controls remain above the card bottom");
        const int statusReserve = MulDiv(kStatusCardGapLogical + kStatusRowHeightLogical +
                                         kStatusBottomMarginLogical,
                                         static_cast<int>(item.dpi), 96);
        Check(layout.sendCard.bottom == item.height - statusReserve,
              "settings cards end at the compact status boundary");

        const auto main = CalculateMainLayoutGeometry(item.width, item.height, item.dpi,
                                                       MulDiv(450, static_cast<int>(item.dpi), 96));
        Check(main.settings.serialCard.top == main.commRecordCard.top,
              "main top baseline is shared");
        Check(main.settings.receiveCard.bottom == main.commRecordCard.bottom,
              "communication bottom aligns with receive settings bottom");
        Check(main.settings.sendCard.top == main.dataSendCard.top,
              "data send top aligns with send settings top");
        Check(main.settings.sendCard.bottom == main.dataSendCard.bottom,
              "main bottom baseline is shared");
        Check(main.statusLeft.top - main.dataSendCard.bottom ==
                  MulDiv(kStatusCardGapLogical, static_cast<int>(item.dpi), 96) &&
              Height(main.statusLeft) ==
                  MulDiv(kStatusRowHeightLogical, static_cast<int>(item.dpi), 96) &&
              item.height - main.statusLeft.bottom ==
                  MulDiv(kStatusBottomMarginLogical, static_cast<int>(item.dpi), 96),
              "status row uses the compact 4/24/6 vertical budget");
        Check(main.settings.receiveCard.top - main.settings.serialCard.bottom == main.settings.cardGap &&
                  main.settings.sendCard.top - main.settings.receiveCard.bottom == main.settings.cardGap &&
                  main.dataSendCard.top - main.commRecordCard.bottom == main.settings.cardGap,
              "left and right card gaps are shared");
        Check(main.commRecordCard.bottom > main.commRecordCard.top &&
                  main.dataSendCard.bottom > main.dataSendCard.top,
              "right cards have positive heights");
    }
    for (const int logicalWidth : {260, 270, 280}) {
        const auto settings = CalculateSettingsGeometry(20, 82, logicalWidth, 1024, 96);
        const int mainAtDefault = 1536 - 2 * 20 - logicalWidth -
                                  2 * kHorizontalCardGapLogical - kExtensionFullWidthLogical;
        std::cout << "CandidateWidth=" << logicalWidth
                  << " InternalMarginLeft=" << settings.serialFields[0].left - settings.serialCard.left
                  << " InternalMarginRight=" << settings.serialCard.right - settings.serialFields[0].right
                  << " SerialComboWidth=" << settings.serialFields[0].right - settings.serialFields[0].left
                  << " EncodingComboWidth=" << settings.encodingCombo.right - settings.encodingCombo.left
                  << " TimedRowRemaining=" << settings.timed.right - settings.timed.left
                  << " MainColumnWidthAtDefault=" << mainAtDefault << '\n';
    }
    CheckResponsiveLayout();
    if (failures == 0) std::cout << "CustomRowGeometryTests=Passed\nContinuousWidthBudgetTests=Passed\nAll UI geometry tests passed.\n";
    return failures == 0 ? 0 : 1;
}
