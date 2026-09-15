#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace protocol {

enum class Type { ModbusRtu };

enum class Function : std::uint8_t {
    ReadCoils = 0x01,
    ReadDiscreteInputs = 0x02,
    ReadHoldingRegisters = 0x03,
    ReadInputRegisters = 0x04,
    WriteSingleCoil = 0x05,
    WriteSingleRegister = 0x06,
    WriteMultipleCoils = 0x0F,
    WriteMultipleRegisters = 0x10,
};

struct Request {
    Type type = Type::ModbusRtu;
    Function function = Function::ReadHoldingRegisters;
    // Modbus slave addresses are entered in decimal (1-247). Other numeric
    // fields remain hexadecimal engineering values.
    std::wstring slave = L"1";
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
