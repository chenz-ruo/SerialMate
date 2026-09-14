#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace protocol {

enum class Type { ModbusRtu };

enum class Function : std::uint8_t {
    ReadHoldingRegisters = 0x03,
    ReadInputRegisters = 0x04,
    WriteSingleRegister = 0x06,
    WriteMultipleRegisters = 0x10,
};

struct Request {
    Type type = Type::ModbusRtu;
    Function function = Function::ReadHoldingRegisters;
    std::wstring slave = L"01";
    std::wstring address;
    std::wstring quantity;
    std::wstring value;
    std::wstring data;
};

struct Result {
    std::vector<std::uint8_t> frame;
    std::wstring hex;
    std::wstring error;
    std::uint8_t crcLow = 0;
    std::uint8_t crcHigh = 0;

    explicit operator bool() const { return error.empty() && !frame.empty(); }
};

Result Generate(const Request& request);

}  // namespace protocol
