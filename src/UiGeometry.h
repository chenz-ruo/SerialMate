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

struct MainLayoutGeometry {
    SettingsGeometry settings{};
    RECT commRecordCard{};
    RECT dataSendCard{};
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

    const int baseCardGap = result.compact ? 8 : 12;
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
    const int available = std::max(1, clientHeight - top - 60);
    const double fitScale = static_cast<double>(available) / totalBase;
    const double scale = std::clamp(std::min(desiredScale, fitScale), 0.85, 1.5);
    result.scalePercent = static_cast<int>(std::lround(scale * 100.0));
    const auto s = [&](int value) { return std::max(1, static_cast<int>(std::lround(value * scale))); };

    const int cardGap = s(baseCardGap);
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
    return result;
}

inline MainLayoutGeometry CalculateMainLayoutGeometry(int clientWidth, int clientHeight,
                                                      unsigned dpi) {
    MainLayoutGeometry result;
    constexpr int pad = 20;
    constexpr int horizontalGap = 12;
    constexpr int top = 16;
    constexpr int toolbarHeight = 46;

    // Keep the settings column compact while preserving enough width for the
    // longest labels and the serial-port selector. At wide windows this is
    // about three Chinese glyphs narrower than the previous 365px column.
    const int leftWidth = std::clamp(clientWidth / 4, 300, 315);
    result.contentTop = top + toolbarHeight + horizontalGap;
    result.horizontalGap = horizontalGap;
    result.settings = CalculateSettingsGeometry(pad, result.contentTop, leftWidth,
                                                 clientHeight, dpi);
    result.contentBottom = result.settings.sendCard.bottom;
    result.rightX = pad + leftWidth + horizontalGap;
    result.rightWidth = std::max(1, clientWidth - result.rightX - pad);

    const int availableHeight = std::max(1, result.contentBottom - result.contentTop);
    const int cardGap = result.settings.cardGap;
    const int sendHeight = std::clamp(availableHeight * 34 / 100, 220, 300);
    const int logBottom = result.contentBottom - sendHeight - cardGap;
    result.commRecordCard = MakeRect(result.rightX, result.contentTop,
                                     result.rightWidth, std::max(1, logBottom - result.contentTop));
    result.dataSendCard = MakeRect(result.rightX, logBottom + cardGap,
                                   result.rightWidth, sendHeight);
    return result;
}
