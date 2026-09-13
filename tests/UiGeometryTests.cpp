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
}  // namespace

int main() {
    struct Case { int width; int height; unsigned dpi; };
    const std::array<Case, 12> cases{{
        {1536, 1024, 96}, {1920, 1080, 96}, {1366, 768, 96}, {1280, 720, 96},
        {1536, 1024, 120}, {1920, 1080, 120}, {1366, 768, 120}, {1280, 720, 120},
        {1536, 1024, 144}, {1920, 1080, 144}, {1366, 768, 144}, {1280, 720, 144},
    }};
    for (const auto& item : cases) {
        const int leftWidth = std::clamp(item.width / 4, 300, 365);
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
        Check(sendBottomPadding >= 8 && sendBottomPadding <= 24,
              "send card bottom padding is compact");
        Check(layout.sendCard.bottom <= item.height - 40,
              "settings cards fit above status bar");

        const auto main = CalculateMainLayoutGeometry(item.width, item.height, item.dpi);
        Check(main.settings.serialCard.top == main.commRecordCard.top,
              "main top baseline is shared");
        Check(main.settings.sendCard.bottom == main.dataSendCard.bottom,
              "main bottom baseline is shared");
        Check(main.settings.receiveCard.top - main.settings.serialCard.bottom == main.settings.cardGap &&
                  main.settings.sendCard.top - main.settings.receiveCard.bottom == main.settings.cardGap &&
                  main.dataSendCard.top - main.commRecordCard.bottom == main.settings.cardGap,
              "left and right card gaps are shared");
        Check(main.commRecordCard.bottom > main.commRecordCard.top &&
                  main.dataSendCard.bottom > main.dataSendCard.top,
              "right cards have positive heights");
    }
    if (failures == 0) std::cout << "All UI geometry tests passed.\n";
    return failures == 0 ? 0 : 1;
}
