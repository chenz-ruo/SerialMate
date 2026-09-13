#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace util {

std::wstring ExecutableDirectory();
std::wstring Timestamp(bool milliseconds = true);
std::wstring FormatBytes(const std::vector<std::uint8_t>& bytes);
std::wstring FormatAsciiBytes(const std::vector<std::uint8_t>& bytes);
std::wstring BytesToDisplayText(const std::vector<std::uint8_t>& bytes);
bool ParseHex(const std::wstring& text, std::vector<std::uint8_t>& output, std::wstring& error);
std::vector<std::uint8_t> Utf8Bytes(const std::wstring& text);
std::wstring Utf8Text(const std::vector<std::uint8_t>& bytes);
std::wstring Win32Error(DWORD code);
std::wstring EscapeText(const std::wstring& text);
bool IsNewerVersion(const std::wstring& candidate, const std::wstring& current);

}

