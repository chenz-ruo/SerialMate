#include "Utilities.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <sstream>

namespace util {

std::wstring ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(count);
    if (!path.empty()) {
        PathRemoveFileSpecW(path.data());
        path.resize(wcslen(path.c_str()));
    }
    return path;
}

std::wstring Timestamp(bool milliseconds) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[64]{};
    if (milliseconds) {
        swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    } else {
        swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond);
    }
    return buffer;
}

std::wstring FormatBytes(const std::vector<std::uint8_t>& bytes) {
    std::wostringstream stream;
    stream << std::uppercase << std::hex << std::setfill(L'0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) stream << L' ';
        stream << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return stream.str();
}

std::wstring FormatAsciiBytes(const std::vector<std::uint8_t>& bytes) {
    std::wstring text;
    text.reserve(bytes.size());
    for (const auto byte : bytes) {
        text.push_back(byte >= 0x20 && byte <= 0x7e ? static_cast<wchar_t>(byte) : L'.');
    }
    return text;
}

std::wstring BytesToDisplayText(const std::vector<std::uint8_t>& bytes) {
    std::wstring text = Utf8Text(bytes);
    if (text.empty() && !bytes.empty()) {
        const int count = MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(bytes.data()),
                                              static_cast<int>(bytes.size()), nullptr, 0);
        if (count > 0) {
            text.resize(count);
            MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(bytes.data()),
                                static_cast<int>(bytes.size()), text.data(), count);
        }
    }
    return EscapeText(text);
}

bool ParseHex(const std::wstring& text, std::vector<std::uint8_t>& output, std::wstring& error) {
    output.clear();
    std::wstring compact;
    compact.reserve(text.size());
    for (const wchar_t ch : text) {
        if (!iswspace(ch) && ch != L',' && ch != L'-') compact.push_back(ch);
    }
    if (compact.rfind(L"0x", 0) == 0 || compact.rfind(L"0X", 0) == 0) compact.erase(0, 2);
    for (std::size_t pos = 0; (pos = compact.find(L"0x", pos)) != std::wstring::npos;) compact.erase(pos, 2);
    for (std::size_t pos = 0; (pos = compact.find(L"0X", pos)) != std::wstring::npos;) compact.erase(pos, 2);
    if (compact.empty()) return true;
    if ((compact.size() % 2) != 0) {
        error = L"十六进制字符数必须为偶数。";
        return false;
    }
    for (std::size_t i = 0; i < compact.size(); i += 2) {
        const auto hi = iswdigit(compact[i]) ? compact[i] - L'0' : towupper(compact[i]) - L'A' + 10;
        const auto lo = iswdigit(compact[i + 1]) ? compact[i + 1] - L'0' : towupper(compact[i + 1]) - L'A' + 10;
        if (hi < 0 || hi > 15 || lo < 0 || lo > 15) {
            error = L"包含无效的十六进制字符。";
            output.clear();
            return false;
        }
        output.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    error.clear();
    return true;
}

std::vector<std::uint8_t> Utf8Bytes(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::vector<std::uint8_t> result(static_cast<std::size_t>(count));
    if (count > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            reinterpret_cast<char*>(result.data()), count, nullptr, nullptr);
    }
    return result;
}

std::wstring Utf8Text(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          reinterpret_cast<const char*>(bytes.data()),
                                          static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes.data()),
                        static_cast<int>(bytes.size()), result.data(), count);
    return result;
}

std::wstring Win32Error(DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = message ? message : L"未知错误";
    if (message) LocalFree(message);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
        result.pop_back();
    }
    return result;
}

std::wstring EscapeText(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (const wchar_t ch : text) {
        switch (ch) {
        case L'\r': result += L"\\r"; break;
        case L'\n': result += L"\\n"; break;
        case L'\t': result += L"\\t"; break;
        default:
            if (ch == L'\0') result += L"\\0";
            else if (ch >= 32) result.push_back(ch);
            else result.push_back(L'.');
            break;
        }
    }
    return result;
}

bool IsNewerVersion(const std::wstring& candidate, const std::wstring& current) {
    const auto parse = [](const std::wstring& value, std::vector<unsigned>& parts) {
        parts.clear();
        if (value.empty() || value.size() > 32) return false;
        unsigned part = 0;
        bool hasDigit = false;
        for (const wchar_t ch : value) {
            if (ch >= L'0' && ch <= L'9') {
                hasDigit = true;
                if (part > 1000000) return false;
                part = part * 10 + static_cast<unsigned>(ch - L'0');
            } else if (ch == L'.' && hasDigit) {
                parts.push_back(part);
                part = 0;
                hasDigit = false;
            } else return false;
        }
        if (!hasDigit) return false;
        parts.push_back(part);
        return parts.size() >= 2 && parts.size() <= 4;
    };
    std::vector<unsigned> left;
    std::vector<unsigned> right;
    if (!parse(candidate, left) || !parse(current, right)) return false;
    const std::size_t count = std::max(left.size(), right.size());
    left.resize(count, 0);
    right.resize(count, 0);
    return left > right;
}

}

