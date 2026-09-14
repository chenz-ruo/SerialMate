#pragma once

// Separate ranges prevent slot arithmetic from resolving to another control type.
namespace extension {
inline constexpr int CustomTitle = 600;
inline constexpr int ProtocolTitle = 601;
inline constexpr int IndexFirst = 610;
inline constexpr int EditFirst = 630;
inline constexpr int SendFirst = 650;
}
