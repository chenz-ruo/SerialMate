#pragma once

// Separate ranges prevent slot arithmetic from resolving to another control type.
namespace extension {
inline constexpr int CustomTitle = 600;
inline constexpr int ProtocolTitle = 601;
inline constexpr int IndexFirst = 610;
inline constexpr int EditFirst = 630;
inline constexpr int SendFirst = 650;
}

namespace protocolui {
inline constexpr int TypeLabel = 700;
inline constexpr int TypeCombo = 701;
inline constexpr int SlaveLabel = 702;
inline constexpr int SlaveEdit = 703;
inline constexpr int FunctionLabel = 704;
inline constexpr int FunctionCombo = 705;
inline constexpr int AddressLabel = 706;
inline constexpr int AddressEdit = 707;
inline constexpr int QuantityLabel = 708;
inline constexpr int QuantityEdit = 709;
inline constexpr int DataLabel = 710;
inline constexpr int DataEdit = 711;
inline constexpr int Generate = 712;
inline constexpr int FillCustom = 713;
inline constexpr int ResultLabel = 714;
inline constexpr int ResultEdit = 715;
inline constexpr int Copy = 716;
inline constexpr int Status = 717;
inline constexpr int FillSlotFirst = 720;
}
