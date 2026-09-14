#pragma once

#include "ProtocolGenerator.h"

#include <cstdint>
#include <vector>

namespace protocol::modbus {

std::uint16_t CalculateCrc(const std::vector<std::uint8_t>& bytes);
Result Generate(const Request& request);

}  // namespace protocol::modbus
