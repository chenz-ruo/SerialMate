#include "../src/Utilities.h"

#include <iostream>

namespace {
int failures = 0;

void Check(bool condition, const wchar_t* name) {
    if (!condition) {
        std::wcerr << L"FAILED: " << name << L'\n';
        ++failures;
    }
}
}

int wmain() {
    std::vector<std::uint8_t> bytes;
    std::wstring error;
    Check(util::ParseHex(L"48 65 6c 6C 6f", bytes, error), L"带空格 HEX 解析");
    Check(bytes == std::vector<std::uint8_t>({0x48, 0x65, 0x6c, 0x6c, 0x6f}), L"HEX 解析结果");
    Check(util::ParseHex(L"0x41,0x54,0D-0A", bytes, error), L"多分隔符 HEX 解析");
    Check(bytes == std::vector<std::uint8_t>({0x41, 0x54, 0x0d, 0x0a}), L"多分隔符结果");
    Check(!util::ParseHex(L"ABC", bytes, error), L"拒绝奇数位 HEX");
    Check(!util::ParseHex(L"GG", bytes, error), L"拒绝无效 HEX");
    const std::wstring chinese = L"SerialMate 串口助手";
    Check(util::Utf8Text(util::Utf8Bytes(chinese)) == chinese, L"UTF-8 往返");
    Check(util::EscapeText(L"A\r\nB\t") == L"A\\r\\nB\\t", L"控制字符显示");
    Check(util::EscapeText(std::wstring(L"A\0B", 3)) == L"A\\0B", L"NUL 字符转义");
    Check(util::FormatBytes({0x00, 0x0a, 0xff}) == L"00 0A FF", L"HEX 格式化");
    Check(util::IsNewerVersion(L"1.0.1", L"1.0.0"), L"新版本判断");
    Check(util::IsNewerVersion(L"2.0", L"1.9.9"), L"主版本判断");
    Check(!util::IsNewerVersion(L"0.9.9", L"1.0.0"), L"旧版本忽略");
    Check(!util::IsNewerVersion(L"<html>", L"1.0.0"), L"非法版本忽略");
    if (failures == 0) std::wcout << L"All utility tests passed.\n";
    return failures == 0 ? 0 : 1;
}
