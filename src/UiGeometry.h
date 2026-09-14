#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>

struct SettingsGeometry {
    RECT serialCard{};
    RECT receiveCard{};
    RECT sendCard{};
    RECT serialTitle{};
    std::array<RECT, 6> serialLabels{};
    std::array<RECT, 6> serialFields{};
    RECT receiveTitle{};
    RECT timestamp{};
    RECT autoScroll{};
    RECT simpleMode{};
    RECT receiveOnly{};
    RECT receiveHex{};
    RECT sendTitle{};
    RECT txHex{};
    RECT txCr{};
    RECT txLf{};
    RECT timed{};
    RECT interval{};
    RECT milliseconds{};
    RECT encodingLabel{};
    RECT encodingCombo{};
    int cardGap = 0;
    int scalePercent = 100;
    bool compact = false;
};

inline constexpr int kMaximumStoredCustomSlots = 16;
inline constexpr int kV110LeftColumnWidthLogical = 300;
inline constexpr int kLeftColumnWidthLogical = 219;
inline constexpr int kExtensionFullWidthLogical = 315;
inline constexpr int kHorizontalCardGapLogical = 8;
inline constexpr int kVerticalCardGapLogical = 8;
inline constexpr int kTopButtonGapLogical = 12;

enum class WidthPhase { Base, Expanding, Expanded };

struct CustomSlotGeometry {
    RECT row{};
    RECT index{};
    RECT edit{};
    RECT send{};
};

struct MainLayoutGeometry {
    SettingsGeometry settings{};
    WidthPhase phase = WidthPhase::Base;
    RECT serialColumn{};
    RECT commRecordCard{};
    RECT dataSendCard{};
    RECT commRecordTitle{};
    RECT dataSendTitle{};
    RECT openButton{};
    RECT closeButton{};
    RECT newButton{};
    RECT logButton{};
    RECT aboutButton{};
    int currentVisibleContentRight = 0;
    bool extensionVisible = false;
    RECT extensionColumn{};
    RECT extensionViewport{};
    RECT extensionFullColumn{};
    RECT customDataCard{};
    RECT protocolCard{};
    RECT customDataTitle{};
    RECT protocolTitle{};
    std::array<CustomSlotGeometry, kMaximumStoredCustomSlots> customSlots{};
    int visibleCustomRows = 0;
    int leftColumnWidth = 0;
    int mainColumnMinWidth8 = 0;
    int extensionFullWidth = 0;
    int extensionContentMinWidth = 0;
    int defaultClientWidth = 0;
    int extensionStartWidth = 0;
    int extensionFullVisibleWidth = 0;
    int extensionGap = 0;
    int cardPadding = 0;
    int extensionColumnWidth = 0;
    int mainCardGap = 0;
    int contentTop = 0;
    int contentBottom = 0;
    int rightX = 0;
    int rightWidth = 0;
    int horizontalGap = 0;
};

inline RECT MakeRect(int x, int y, int width, int height) {
    return RECT{x, y, x + width, y + height};
}

inline SettingsGeometry CalculateSettingsGeometry(int left, int top, int width,
                                                   int clientHeight, unsigned dpi) {
    SettingsGeometry result;
    const double desiredScale = std::clamp(static_cast<double>(dpi) / 96.0, 1.0, 1.5);
    result.compact = clientHeight < static_cast<int>(900.0 * desiredScale);

    const int baseCardGap = kVerticalCardGapLogical;
    const int baseTopPadding = result.compact ? 8 : 14;
    const int baseBottomPadding = result.compact ? 10 : 16;
    const int baseTitleHeight = result.compact ? 24 : 28;
    const int baseTitleGap = result.compact ? 4 : 10;
    const int baseControlHeight = result.compact ? 26 : 30;
    const int baseComboHeight = result.compact ? 28 : 34;
    const int baseRowGap = result.compact ? 4 : 10;
    const int baseSendRowGap = result.compact ? 2 : 6;
    const int baseGroupGap = result.compact ? 8 : 14;
    const int baseEncodingGap = result.compact ? 4 : 8;

    const int serialBase = baseTopPadding + baseTitleHeight + baseTitleGap +
                           6 * baseComboHeight + 5 * baseRowGap + baseBottomPadding;
    const int receiveBase = baseTopPadding + baseTitleHeight + baseTitleGap +
                            5 * baseControlHeight + 4 * baseRowGap + baseBottomPadding;
    const int sendBase = baseTopPadding + baseTitleHeight + baseTitleGap +
                         4 * baseControlHeight + 3 * baseSendRowGap + baseGroupGap +
                         baseTitleHeight + baseEncodingGap + baseComboHeight + baseBottomPadding;
    const int totalBase = serialBase + receiveBase + sendBase + 2 * baseCardGap;
    // The settings stack and the right-hand cards share the same bottom
    // baseline. Leave the same 40px status-bar band and 20px client padding
    // that MainLayoutGeometry uses, so the initial layout and every resize
    // are calculated from the same available client area.
    // Reserve the status-bar band plus a small physical safety margin so
    // rounded compact dimensions never push the last control into the bar.
    const int available = std::max(1, clientHeight - top - 60 - 8);
    const double fitScale = static_cast<double>(available) / totalBase;
    // Allow the compact stack to fit genuinely short client areas; the old
    // 0.85 floor could push the final card below the status-bar boundary.
    const double scale = std::clamp(std::min(desiredScale, fitScale), 0.75, 1.5);
    result.scalePercent = static_cast<int>(std::lround(scale * 100.0));
    const auto s = [&](int value) { return std::max(1, static_cast<int>(std::lround(value * scale))); };

    // Card-to-card spacing is a design constant; only DPI may scale it.
    const int cardGap = std::max(1, MulDiv(kVerticalCardGapLogical,
                                           static_cast<int>(dpi ? dpi : 96), 96));
    const int topPadding = s(baseTopPadding);
    const int bottomPadding = s(baseBottomPadding);
    const int titleHeight = s(baseTitleHeight);
    const int titleGap = s(baseTitleGap);
    const int controlHeight = s(baseControlHeight);
    const int comboHeight = s(baseComboHeight);
    const int rowGap = s(baseRowGap);
    const int sendRowGap = s(baseSendRowGap);
    const int groupGap = s(baseGroupGap);
    const int encodingGap = s(baseEncodingGap);
    const int titleX = left + s(12);
    const int controlX = left + s(18);
    const int rightPadding = s(12);
    const int innerWidth = width - s(36);
    result.cardGap = cardGap;

    int y = top;
    int contentY = y + topPadding;
    result.serialTitle = MakeRect(titleX, contentY, width - s(24), titleHeight);
    contentY = result.serialTitle.bottom + titleGap;
    const int labelWidth = s(58);
    const int fieldX = left + s(76);
    const int fieldWidth = left + width - rightPadding - fieldX;
    for (std::size_t index = 0; index < result.serialFields.size(); ++index) {
        // Native combo boxes have a slightly taller outer frame than their
        // text area. Keep the label on the same row, but use the actual label
        // control height and center it against the combo's client area.
        const int labelHeight = controlHeight;
        const int labelOffset = std::max(0, (comboHeight - labelHeight) / 2);
        result.serialLabels[index] = MakeRect(titleX, contentY + labelOffset,
                                              labelWidth, labelHeight);
        result.serialFields[index] = MakeRect(fieldX, contentY, fieldWidth, comboHeight);
        contentY += comboHeight + (index + 1 < result.serialFields.size() ? rowGap : 0);
    }
    result.serialCard = MakeRect(left, y, width, contentY + bottomPadding - y);

    y = result.serialCard.bottom + cardGap;
    contentY = y + topPadding;
    result.receiveTitle = MakeRect(titleX, contentY, width - s(24), titleHeight);
    contentY = result.receiveTitle.bottom + titleGap;
    result.receiveHex = MakeRect(controlX, contentY, innerWidth, controlHeight);
    contentY += controlHeight + rowGap;
    result.timestamp = MakeRect(controlX, contentY, innerWidth, controlHeight);
    contentY += controlHeight + rowGap;
    result.autoScroll = MakeRect(controlX, contentY, innerWidth, controlHeight);
    contentY += controlHeight + rowGap;
    result.receiveOnly = MakeRect(controlX, contentY, innerWidth, controlHeight);
    contentY += controlHeight + rowGap;
    result.simpleMode = MakeRect(controlX, contentY, innerWidth, controlHeight);
    contentY += controlHeight;
    result.receiveCard = MakeRect(left, y, width, contentY + bottomPadding - y);

    y = result.receiveCard.bottom + cardGap;
    contentY = y + topPadding;
    result.sendTitle = MakeRect(titleX, contentY, width - s(24), titleHeight);
    contentY = result.sendTitle.bottom + titleGap;
    result.txHex = MakeRect(controlX, contentY, innerWidth, controlHeight);
    contentY += controlHeight + sendRowGap;
    result.txCr = MakeRect(controlX, contentY, innerWidth, controlHeight);
    contentY += controlHeight + sendRowGap;
    result.txLf = MakeRect(controlX, contentY, innerWidth, controlHeight);
    contentY += controlHeight + sendRowGap;
    const int timedRowRight = left + width - rightPadding;
    const int timedGap = s(6);
    const int millisecondsWidth = s(28);
    const int intervalWidth = s(64);
    // Anchor the unit inside the card first; allocate the remaining row to
    // the checkbox instead of forcing a minimum width that can overflow.
    const int millisecondsX = timedRowRight - millisecondsWidth;
    const int intervalX = millisecondsX - timedGap - intervalWidth;
    result.timed = MakeRect(controlX, contentY, intervalX - timedGap - controlX, controlHeight);
    result.interval = MakeRect(intervalX, contentY, intervalWidth, controlHeight);
    result.milliseconds = MakeRect(millisecondsX, contentY,
                                   millisecondsWidth, controlHeight);
    contentY += controlHeight + groupGap;
    result.encodingLabel = MakeRect(titleX, contentY, width - s(24), titleHeight);
    contentY += titleHeight + encodingGap;
    result.encodingCombo = MakeRect(controlX, contentY, innerWidth, comboHeight);
    contentY += comboHeight;
    result.sendCard = MakeRect(left, y, width, contentY + bottomPadding - y);
    // Keep the final card one physical pixel above the status-bar band. The
    // scaled stack uses rounded dimensions, so without this clamp a 1px
    // rounding carry can let the bottom border be clipped by the parent.
    const int statusTop = clientHeight - MulDiv(40, static_cast<int>(dpi ? dpi : 96), 96);
    result.sendCard.bottom = static_cast<LONG>(std::min(static_cast<int>(result.sendCard.bottom),
                                                       statusTop - 1));
    return result;
}

inline MainLayoutGeometry CalculateMainLayoutGeometry(int clientWidth, int clientHeight,
                                                      unsigned dpi,
                                                      int recordContentWidth8) {
    MainLayoutGeometry result;
    const auto scale = [dpi](int value) {
        return std::max(1, MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96));
    };

    const int top = scale(16);
    // The toolbar button keeps the v1.1.0 physical height (52px). Reserve
    // exactly that height so the Card starts after one fixed card gap rather
    // than leaving a DPI-dependent empty band below the buttons.
    constexpr int toolbarHeight = 52;
    const int pad = scale(20);
    const int horizontalGap = scale(kHorizontalCardGapLogical);
    const int leftWidth = scale(kLeftColumnWidthLogical);
    result.leftColumnWidth = leftWidth;
    result.cardPadding = scale(12);
    result.contentTop = top + toolbarHeight + horizontalGap;
    result.horizontalGap = horizontalGap;
    result.settings = CalculateSettingsGeometry(pad, result.contentTop, leftWidth,
                                                 clientHeight, dpi);
    result.contentBottom = result.settings.sendCard.bottom;
    result.serialColumn = RECT{pad, result.contentTop, pad + leftWidth, result.contentBottom};
    result.rightX = pad + leftWidth + horizontalGap;
    const int availableRight = std::max(1, clientWidth - result.rightX - pad);
    // The measured payload includes timestamp, direction, HEX, divider, TEXT
    // and its own padding. Reserve the native scrollbar and a little breathing
    // room as well as the two card insets; do not infer this from a screen size.
    result.mainColumnMinWidth8 = recordContentWidth8 +
        GetSystemMetricsForDpi(SM_CXVSCROLL, dpi ? dpi : 96) + scale(12) + 2 * result.cardPadding;
    result.extensionFullWidth = scale(kExtensionFullWidthLogical);
    result.defaultClientWidth = result.rightX + result.mainColumnMinWidth8 + pad;
    result.extensionStartWidth = result.defaultClientWidth + 1;
    const int fullBudget = result.extensionFullWidth + horizontalGap;
    result.extensionFullVisibleWidth = result.defaultClientWidth + fullBudget;
    const int extra = std::max(0, availableRight - result.mainColumnMinWidth8);
    // Card gaps are fixed constants, never part of a proportional width
    // allocation. The first gap pixels are reserved before the viewport is
    // revealed; once the full column is visible, all additional width goes
    // exclusively to the middle column.
    result.extensionGap = extra > 0 ? horizontalGap : 0;
    result.extensionColumnWidth = std::clamp(extra - horizontalGap, 0, result.extensionFullWidth);
    result.extensionVisible = result.extensionColumnWidth > 0;
    const int consumed = extra > 0 ? horizontalGap + result.extensionColumnWidth : 0;
    result.rightWidth = result.mainColumnMinWidth8 + std::max(0, extra - consumed);
    result.phase = extra == 0 ? WidthPhase::Base :
        (extra < fullBudget ? WidthPhase::Expanding : WidthPhase::Expanded);

    const int cardGap = result.settings.cardGap;
    result.mainCardGap = cardGap;
    result.commRecordCard = RECT{result.rightX, result.contentTop,
                                 result.rightX + result.rightWidth,
                                 result.settings.receiveCard.bottom};
    result.dataSendCard = RECT{result.rightX, result.settings.sendCard.top,
                               result.rightX + result.rightWidth,
                               result.contentBottom};
    result.openButton = MakeRect(result.settings.serialCard.left, top, 0, 52);
    result.closeButton = result.openButton;
    const int titleTopPadding = result.settings.serialTitle.top - result.settings.serialCard.top;
    const int titleHeight = result.settings.serialTitle.bottom - result.settings.serialTitle.top;
    const int titleGap = result.settings.serialFields[0].top - result.settings.serialTitle.bottom;
    const int innerPadding = result.cardPadding;
    const auto title = [&](const RECT& card) {
        if (card.right - card.left <= 2 * innerPadding) return RECT{};
        return MakeRect(card.left + innerPadding, card.top + titleTopPadding,
                        card.right - card.left - 2 * innerPadding, titleHeight);
    };
    result.commRecordTitle = title(result.commRecordCard);
    result.commRecordTitle.right = std::min(result.commRecordTitle.right, result.commRecordTitle.left + 150);
    result.dataSendTitle = title(result.dataSendCard);
    const int indexWidth = scale(22);
    const int sendWidth = scale(56);
    const int columnGap = scale(6);
    // The extension is a fixed-width design column. During Phase B only its
    // viewport is revealed; controls are laid out at full width and clipped,
    // never compressed into malformed rows.
    result.extensionContentMinWidth = result.extensionFullWidth;
    const auto layoutToolbar = [&]() {
        const int topButtonGap = scale(kTopButtonGapLogical);
        // v1.1.0 used a 52px physical toolbar button height. Keep that
        // visual size instead of enlarging the button at high DPI; width and
        // alignment still come from the scaled Card geometry.
        constexpr int topButtonHeight = 52;
        const int leftWidthForButtons = result.settings.serialCard.right - result.settings.serialCard.left;
        const int openWidth = std::max(1, (leftWidthForButtons - topButtonGap) / 2);
        result.openButton = MakeRect(result.settings.serialCard.left, top, openWidth, topButtonHeight);
        result.closeButton = MakeRect(result.openButton.right + topButtonGap, top,
                                      leftWidthForButtons - openWidth - topButtonGap, topButtonHeight);
        const std::array<int, 3> menuWidths{130, 112, 98};
        int menuRight = result.currentVisibleContentRight;
        const auto setMenu = [&](RECT& rect, int width) {
            rect = MakeRect(menuRight - scale(width), top, scale(width), topButtonHeight);
            menuRight = rect.left - topButtonGap;
        };
        setMenu(result.aboutButton, menuWidths[2]);
        setMenu(result.logButton, menuWidths[1]);
        setMenu(result.newButton, menuWidths[0]);
    };
    if (!result.extensionVisible) {
        result.currentVisibleContentRight = result.commRecordCard.right;
        layoutToolbar();
        return result;
    }

    const int extensionLeft = result.commRecordCard.right + result.extensionGap;
    const int extensionRight = extensionLeft + result.extensionColumnWidth;
    result.extensionColumn = RECT{extensionLeft, result.contentTop, extensionRight, result.contentBottom};
    result.currentVisibleContentRight = result.extensionColumn.right;
    result.extensionViewport = result.extensionColumn;
    const int fullRight = extensionLeft + result.extensionFullWidth;
    result.extensionFullColumn = RECT{extensionLeft, result.contentTop, fullRight, result.contentBottom};
    // These are copied from the middle cards, never independently inferred.
    result.customDataCard = RECT{extensionLeft, result.commRecordCard.top,
                                 fullRight, result.commRecordCard.bottom};
    result.protocolCard = RECT{extensionLeft, result.dataSendCard.top,
                               fullRight, result.dataSendCard.bottom};
    result.customDataTitle = title(result.customDataCard);
    result.protocolTitle = title(result.protocolCard);
    const int rowsTop = result.customDataTitle.bottom + titleGap;
    const int rowsBottom = result.customDataCard.bottom - innerPadding;
    const int rowHeight = scale(32);
    const int rowGap = scale(6);
    result.visibleCustomRows = std::clamp((rowsBottom - rowsTop + rowGap) / (rowHeight + rowGap),
                                         0, kMaximumStoredCustomSlots);
    for (int i = 0; i < result.visibleCustomRows; ++i) {
        auto& slot = result.customSlots[static_cast<std::size_t>(i)];
        slot.row = MakeRect(extensionLeft + innerPadding, rowsTop + i * (rowHeight + rowGap),
                             result.extensionFullWidth - 2 * innerPadding, rowHeight);
        slot.index = MakeRect(slot.row.left, slot.row.top, indexWidth, rowHeight);
        slot.send = MakeRect(slot.row.right - sendWidth, slot.row.top, sendWidth, rowHeight);
        slot.edit = RECT{slot.index.right + columnGap, slot.row.top,
                         slot.send.left - columnGap, slot.row.bottom};
    }
    layoutToolbar();
    return result;
}
