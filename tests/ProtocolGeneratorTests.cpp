#include "ModbusRtuGenerator.h"
#include "ProtocolGenerator.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (condition) return;
    ++failures;
    std::wcerr << L"FAIL: " << message << L'\n';
}

protocol::Request Request(protocol::Function function, const wchar_t* address,
                          const wchar_t* quantity = L"", const wchar_t* value = L"",
                          const wchar_t* data = L"") {
    return {protocol::Type::ModbusRtu, function, L"01", address, quantity, value, data};
}

void CheckFrame(protocol::Request request, const wchar_t* expected, std::uint8_t crcLow,
                std::uint8_t crcHigh, const wchar_t* message) {
    const auto result = protocol::Generate(request);
    Check(static_cast<bool>(result), L"标准帧生成失败");
    Check(result.hex == expected, message);
    Check(result.crcLow == crcLow && result.crcHigh == crcHigh, L"CRC 字节返回错误");
    Check(result.frame.size() >= 2 && result.frame[result.frame.size() - 2] == crcLow &&
              result.frame.back() == crcHigh,
          L"CRC 没有按低字节在前附加");
}

void ExpectInvalid(protocol::Request request, const wchar_t* message) {
    const auto result = protocol::Generate(request);
    Check(!static_cast<bool>(result), message);
    Check(result.frame.empty() && result.hex.empty(), L"非法参数产生了部分帧");
    Check(!result.error.empty(), L"非法参数没有返回错误信息");
}

void CheckStandardFrames() {
    CheckFrame(Request(protocol::Function::ReadHoldingRegisters, L"0000", L"0002"),
               L"01 03 00 00 00 02 C4 0B", 0xC4, 0x0B, L"03 标准帧不匹配");
    CheckFrame(Request(protocol::Function::ReadInputRegisters, L"0000", L"0001"),
               L"01 04 00 00 00 01 31 CA", 0x31, 0xCA, L"04 标准帧不匹配");
    CheckFrame(Request(protocol::Function::WriteSingleRegister, L"0001", L"", L"0003"),
               L"01 06 00 01 00 03 98 0B", 0x98, 0x0B, L"06 标准帧不匹配");
    CheckFrame(Request(protocol::Function::WriteMultipleRegisters, L"0001", L"0002", L"",
                       L"000A 0102"),
               L"01 10 00 01 00 02 04 00 0A 01 02 92 30", 0x92, 0x30,
               L"10 标准帧不匹配");

    auto normalized = Request(protocol::Function::WriteMultipleRegisters, L"a", L"2", L"",
                              L"000a\r\n0102");
    normalized.slave = L" 1 ";
    Check(protocol::Generate(normalized).hex ==
              L"01 10 00 0A 00 02 04 00 0A 01 02 D3 83",
          L"合法小写和空白没有规范化");
}

void CheckCrcVector() {
    const std::vector<std::uint8_t> payload{0x01, 0x03, 0x00, 0x00, 0x00, 0x02};
    Check(protocol::modbus::CalculateCrc(payload) == 0x0BC4, L"CRC-16 标准向量错误");
}

void CheckInvalidParameters() {
    for (const wchar_t* slave : {L"", L"00", L"F8", L"GG", L"001"}) {
        auto request = Request(protocol::Function::ReadHoldingRegisters, L"0000", L"0001");
        request.slave = slave;
        ExpectInvalid(request, L"非法从机地址被接受");
    }

    for (const wchar_t* address : {L"", L"10000", L"-1", L"12G4"})
        ExpectInvalid(Request(protocol::Function::ReadHoldingRegisters, address, L"0001"),
                      L"非法寄存器地址被接受");

    for (const wchar_t* quantity : {L"", L"0000", L"007E", L"10000", L"ZZ"})
        ExpectInvalid(Request(protocol::Function::ReadInputRegisters, L"0000", quantity),
                      L"非法读取数量被接受");

    for (const wchar_t* value : {L"", L"10000", L"XYZ"})
        ExpectInvalid(Request(protocol::Function::WriteSingleRegister, L"0001", L"", value),
                      L"非法单寄存器值被接受");

    for (const wchar_t* quantity : {L"", L"0000", L"007C", L"10000", L"QQ"})
        ExpectInvalid(Request(protocol::Function::WriteMultipleRegisters, L"0001", quantity,
                              L"", L"0001"),
                      L"非法多寄存器数量被接受");

    ExpectInvalid(Request(protocol::Function::WriteMultipleRegisters, L"0001", L"0002", L"",
                          L"000A 010"),
                  L"半个数据 word 被接受");
    ExpectInvalid(Request(protocol::Function::WriteMultipleRegisters, L"0001", L"0002", L"",
                          L"000A"),
                  L"数据 word 数量不匹配被接受");
    ExpectInvalid(Request(protocol::Function::WriteMultipleRegisters, L"0001", L"0001", L"",
                          L"00XZ"),
                  L"非法数据 HEX 被接受");
}
}  // namespace

int wmain() {
    CheckCrcVector();
    CheckStandardFrames();
    CheckInvalidParameters();
    if (failures == 0) std::wcout << L"All protocol generator tests passed.\n";
    return failures == 0 ? 0 : 1;
}
