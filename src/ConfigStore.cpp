#include "ConfigStore.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

namespace config {
namespace {

struct ParsedConfig {
    ConfigData data{};
    std::bitset<kSlotCount> present{};
};

class MutexLock final {
public:
    explicit MutexLock(const std::wstring& name) {
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle_) return;
        const DWORD wait = WaitForSingleObject(handle_, INFINITE);
        acquired_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }
    ~MutexLock() {
        if (acquired_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }
    bool Acquired() const { return acquired_; }

private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
};

bool Exists(const std::filesystem::path& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring Win32Error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = length && buffer ? std::wstring(buffer, length) : L"错误代码 " + std::to_wstring(code);
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}

bool ReadBytes(const std::filesystem::path& path, std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    const bool validSize = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= 1024 * 1024;
    if (!validSize) {
        CloseHandle(file);
        return false;
    }
    bytes.assign(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = bytes.empty() ||
        (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size());
    CloseHandle(file);
    return ok;
}

bool DecodeUtf8(const std::string& input, std::wstring& output) {
    if (input.empty()) {
        output.clear();
        return true;
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) return false;
    output.resize(static_cast<std::size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                               static_cast<int>(input.size()), output.data(), count) == count;
}

bool EncodeUtf8(const std::wstring& input, std::string& output) {
    if (input.empty()) {
        output.clear();
        return true;
    }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return false;
    output.resize(static_cast<std::size_t>(count));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                               static_cast<int>(input.size()), output.data(), count, nullptr, nullptr) == count;
}

int SlotFromKey(const std::string& key) {
    if (key.size() != 12 || key.compare(0, 10, "CustomData") != 0 ||
        key[10] < '0' || key[10] > '9' || key[11] < '0' || key[11] > '9') return -1;
    const int number = (key[10] - '0') * 10 + key[11] - '0';
    return number >= 1 && number <= static_cast<int>(kSlotCount) ? number - 1 : -1;
}

ParsedConfig ReadConfig(const std::filesystem::path& path, const ConfigData& base = {}) {
    ParsedConfig parsed;
    parsed.data = base;
    std::string bytes;
    if (!ReadBytes(path, bytes)) return parsed;
    std::size_t start = 0;
    while (start <= bytes.size()) {
        const std::size_t end = bytes.find('\n', start);
        std::string line = bytes.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t separator = line.find('=');
        if (separator != std::string::npos) {
            const int slot = SlotFromKey(line.substr(0, separator));
            if (slot >= 0) {
                std::wstring value;
                if (DecodeUtf8(line.substr(separator + 1), value) &&
                    value.size() <= kMaximumCustomDataLength) {
                    parsed.data.customData[static_cast<std::size_t>(slot)] = std::move(value);
                    parsed.present.set(static_cast<std::size_t>(slot));
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parsed;
}

bool EnsureParent(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    return !error;
}

bool CanPersistTo(const std::filesystem::path& path) {
    if (Exists(path)) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_READONLY) != 0) return false;
        HANDLE writable = CreateFileW(path.c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (writable == INVALID_HANDLE_VALUE) return false;
        CloseHandle(writable);
    }
    if (!std::filesystem::exists(path.parent_path())) return false;
    const auto probe = path.parent_path() /
        (L"SerialMate.ini.probe." + std::to_wstring(GetCurrentProcessId()));
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    DeleteFileW(probe.c_str());
    return true;
}

std::wstring MutexName(const std::filesystem::path& path) {
    std::error_code error;
    std::wstring normalized = std::filesystem::absolute(path, error).lexically_normal().wstring();
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), towlower);
    std::uint64_t hash = 1469598103934665603ULL;
    for (const wchar_t ch : normalized) {
        hash ^= static_cast<std::uint16_t>(ch);
        hash *= 1099511628211ULL;
    }
    std::wostringstream output;
    output << L"Local\\SerialMate.Config." << std::hex << hash;
    return output.str();
}

bool Serialize(const ConfigData& data, std::string& bytes, std::wstring& error) {
    bytes = "ConfigVersion=1\r\n";
    for (std::size_t index = 0; index < kSlotCount; ++index) {
        const auto& value = data.customData[index];
        if (value.size() > kMaximumCustomDataLength ||
            value.find_first_of(L"\r\n") != std::wstring::npos) {
            error = L"自定义数据 " + std::to_wstring(index + 1) + L" 长度或格式无效。";
            return false;
        }
        std::string encoded;
        if (!EncodeUtf8(value, encoded)) {
            error = L"自定义数据 " + std::to_wstring(index + 1) + L" 无法编码为 UTF-8。";
            return false;
        }
        const int number = static_cast<int>(index + 1);
        bytes += "CustomData";
        bytes.push_back(static_cast<char>('0' + number / 10));
        bytes.push_back(static_cast<char>('0' + number % 10));
        bytes.push_back('=');
        bytes += encoded;
        bytes += "\r\n";
    }
    return true;
}

bool WriteAtomic(const std::filesystem::path& target, const std::string& bytes, std::wstring& error) {
    if (!EnsureParent(target)) {
        error = L"无法创建配置目录。";
        return false;
    }
    const std::filesystem::path temporary = target.wstring() + L".tmp." +
                                            std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法创建配置临时文件：" + Win32Error(GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool wrote = bytes.empty() ||
        (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
         written == bytes.size());
    const bool flushed = wrote && FlushFileBuffers(file) != FALSE;
    const DWORD ioError = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!flushed) {
        DeleteFileW(temporary.c_str());
        error = L"无法完整写入配置：" + Win32Error(ioError);
        return false;
    }

    const DWORD previousAttributes = GetFileAttributesW(target.c_str());
    bool replaced = false;
    if (previousAttributes != INVALID_FILE_ATTRIBUTES) {
        replaced = ReplaceFileW(target.c_str(), temporary.c_str(), nullptr,
                                REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE;
    } else {
        replaced = MoveFileExW(temporary.c_str(), target.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!replaced) {
        const DWORD replaceError = GetLastError();
        DeleteFileW(temporary.c_str());
        error = L"无法替换配置文件：" + Win32Error(replaceError);
        return false;
    }
    DWORD attributes = previousAttributes == INVALID_FILE_ATTRIBUTES ? 0 : previousAttributes;
    attributes &= ~FILE_ATTRIBUTE_NORMAL;
    attributes |= FILE_ATTRIBUTE_HIDDEN;
    if (!SetFileAttributesW(target.c_str(), attributes)) {
        error = L"无法隐藏配置文件：" + Win32Error(GetLastError());
        return false;
    }
    return true;
}

} // namespace

ConfigStore ConfigStore::Open(const std::filesystem::path& primary,
                              const std::filesystem::path& fallback) {
    ConfigData data{};
    if (Exists(primary)) {
        data = ReadConfig(primary).data;
        if (CanPersistTo(primary)) return ConfigStore(primary, std::move(data), false);
        if (Exists(fallback)) data = ReadConfig(fallback, data).data;
        EnsureParent(fallback);
        return ConfigStore(fallback, std::move(data), true);
    }
    if (Exists(fallback)) return ConfigStore(fallback, ReadConfig(fallback).data, true);
    if (CanPersistTo(primary)) return ConfigStore(primary, {}, false);
    EnsureParent(fallback);
    return ConfigStore(fallback, {}, true);
}

ConfigStore ConfigStore::OpenDefault() {
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    module.resize(length);
    const auto primary = std::filesystem::path(module).parent_path() / L"SerialMate.ini";
    PWSTR localPath = nullptr;
    std::filesystem::path fallback;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localPath))) {
        fallback = std::filesystem::path(localPath) / L"SerialMate" / L"SerialMate.ini";
        CoTaskMemFree(localPath);
    } else {
        wchar_t environment[32768]{};
        const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", environment,
                                                     static_cast<DWORD>(std::size(environment)));
        fallback = count > 0 && count < std::size(environment)
            ? std::filesystem::path(environment) / L"SerialMate" / L"SerialMate.ini"
            : primary;
    }
    return Open(primary, fallback);
}

bool ConfigStore::SaveMerged(const ConfigData& data, const std::bitset<kSlotCount>& dirty,
                             std::wstring& error) {
    MutexLock lock(MutexName(path_));
    if (!lock.Acquired()) {
        error = L"无法锁定配置文件。";
        return false;
    }
    ConfigData merged = initialData_;
    if (Exists(path_)) merged = ReadConfig(path_, merged).data;
    for (std::size_t index = 0; index < kSlotCount; ++index) {
        if (dirty.test(index)) merged.customData[index] = data.customData[index];
    }
    std::string bytes;
    if (!Serialize(merged, bytes, error) || !WriteAtomic(path_, bytes, error)) return false;
    initialData_ = std::move(merged);
    error.clear();
    return true;
}

} // namespace config
