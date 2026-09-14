#include "ModbusRtuGenerator.h"

#include <cwctype>
#include <optional>
#include <string>

namespace protocol::modbus {
namespace {

bool IsHex(wchar_t value) {
    return (value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f') ||
           (value >= L'A' && value <= L'F');
}

unsigned HexValue(wchar_t value) {
    if (value >= L'0' && value <= L'9') return static_cast<unsigned>(value - L'0');
    return static_cast<unsigned>(std::towupper(value) - L'A' + 10);
}

std::wstring Trim(const std::wstring& text) {
    std::size_t first = 0;
    while (first < text.size() && std::iswspace(text[first])) ++first;
    std::size_t last = text.size();
    while (last > first && std::iswspace(text[last - 1])) --last;
    return text.substr(first, last - first);
}

std::optional<std::uint16_t> ParseScalar(const std::wstring& input, std::size_t maxDigits,
                                         const wchar_t* field, std::wstring& error) {
    const std::wstring text = Trim(input);
    if (text.empty()) {
        error = std::wstring(field) + L"不能为空";
        return std::nullopt;
    }
    if (text.size() > maxDigits) {
        error = std::wstring(field) + L"超出HEX位数范围";
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (wchar_t character : text) {
        if (!IsHex(character)) {
            error = std::wstring(field) + L"包含非法HEX字符";
            return std::nullopt;
        }
        value = value * 16 + HexValue(character);
    }
    return static_cast<std::uint16_t>(value);
}

void PushWord(std::vector<std::uint8_t>& frame, std::uint16_t value) {
    frame.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::optional<std::vector<std::uint16_t>> ParseWords(const std::wstring& input,
                                                      std::wstring& error) {
    std::wstring compact;
    compact.reserve(input.size());
    for (wchar_t character : input) {
        if (std::iswspace(character)) continue;
        if (!IsHex(character)) {
            error = L"写入数据包含非法HEX字符";
            return std::nullopt;
        }
        compact.push_back(character);
    }
    if (compact.empty()) {
        error = L"写入数据不能为空";
        return std::nullopt;
    }
    if ((compact.size() % 4) != 0) {
        error = L"写入数据必须由完整的16位word组成";
        return std::nullopt;
    }
    std::vector<std::uint16_t> words;
    words.reserve(compact.size() / 4);
    for (std::size_t offset = 0; offset < compact.size(); offset += 4) {
        std::uint16_t value = 0;
        for (std::size_t index = 0; index < 4; ++index)
            value = static_cast<std::uint16_t>(value * 16 + HexValue(compact[offset + index]));
        words.push_back(value);
    }
    return words;
}

std::wstring FormatHex(const std::vector<std::uint8_t>& frame) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring text;
    if (!frame.empty()) text.reserve(frame.size() * 3 - 1);
    for (std::size_t index = 0; index < frame.size(); ++index) {
        if (index != 0) text.push_back(L' ');
        text.push_back(digits[(frame[index] >> 4) & 0x0F]);
        text.push_back(digits[frame[index] & 0x0F]);
    }
    return text;
}

Result Failure(std::wstring error) {
    Result result;
    result.error = std::move(error);
    return result;
}

}  // namespace

std::uint16_t CalculateCrc(const std::vector<std::uint8_t>& bytes) {
    std::uint16_t crc = 0xFFFF;
    for (std::uint8_t byte : bytes) {
        crc = static_cast<std::uint16_t>(crc ^ byte);
        for (int bit = 0; bit < 8; ++bit) {
            const bool lowBitSet = (crc & 0x0001) != 0;
            crc = static_cast<std::uint16_t>(crc >> 1);
            if (lowBitSet) crc = static_cast<std::uint16_t>(crc ^ 0xA001);
        }
    }
    return crc;
}

Result Generate(const Request& request) {
    std::wstring error;
    const auto slave = ParseScalar(request.slave, 2, L"从机地址", error);
    if (!slave) return Failure(std::move(error));
    if (*slave < 0x01 || *slave > 0xF7) return Failure(L"从机地址范围必须是01-F7");

    const auto address = ParseScalar(request.address, 4, L"寄存器地址", error);
    if (!address) return Failure(std::move(error));

    std::vector<std::uint8_t> frame{static_cast<std::uint8_t>(*slave),
                                    static_cast<std::uint8_t>(request.function)};
    PushWord(frame, *address);

    switch (request.function) {
    case Function::ReadHoldingRegisters:
    case Function::ReadInputRegisters: {
        const auto quantity = ParseScalar(request.quantity, 4, L"寄存器数量", error);
        if (!quantity) return Failure(std::move(error));
        if (*quantity < 1 || *quantity > 125)
            return Failure(L"读取寄存器数量范围必须是0001-007D");
        PushWord(frame, *quantity);
        break;
    }
    case Function::WriteSingleRegister: {
        const auto value = ParseScalar(request.value, 4, L"写入值", error);
        if (!value) return Failure(std::move(error));
        PushWord(frame, *value);
        break;
    }
    case Function::WriteMultipleRegisters: {
        const auto quantity = ParseScalar(request.quantity, 4, L"寄存器数量", error);
        if (!quantity) return Failure(std::move(error));
        if (*quantity < 1 || *quantity > 123)
            return Failure(L"写入寄存器数量范围必须是0001-007B");
        const auto words = ParseWords(request.data, error);
        if (!words) return Failure(std::move(error));
        if (words->size() != *quantity)
            return Failure(L"写入数据word数量必须与寄存器数量一致");
        PushWord(frame, *quantity);
        frame.push_back(static_cast<std::uint8_t>(*quantity * 2));
        for (std::uint16_t word : *words) PushWord(frame, word);
        break;
    }
    default:
        return Failure(L"不支持的Modbus功能码");
    }

    const std::uint16_t crc = CalculateCrc(frame);
    Result result;
    result.crcLow = static_cast<std::uint8_t>(crc & 0xFF);
    result.crcHigh = static_cast<std::uint8_t>((crc >> 8) & 0xFF);
    frame.push_back(result.crcLow);
    frame.push_back(result.crcHigh);
    result.frame = std::move(frame);
    result.hex = FormatHex(result.frame);
    return result;
}

}  // namespace protocol::modbus
