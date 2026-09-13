#include "Version.h"

#include <windows.h>
#include <winver.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int wmain() {
    std::vector<wchar_t> module(32768);
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (length == 0 || length >= module.size()) return 1;
    const auto executable = std::filesystem::path(module.data()).parent_path() / L"SerialMate.exe";
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(executable.c_str(), &ignored);
    if (bytes == 0) {
        std::wcerr << L"FAILED: missing version resource\n";
        return 1;
    }
    std::vector<std::uint8_t> data(bytes);
    if (!GetFileVersionInfoW(executable.c_str(), 0, bytes, data.data())) return 1;
    void* value = nullptr;
    UINT valueBytes = 0;
    if (!VerQueryValueW(data.data(), L"\\StringFileInfo\\080404b0\\FileVersion",
                        &value, &valueBytes) || !value) return 1;
    const std::wstring fileVersion(static_cast<const wchar_t*>(value));
    if (fileVersion != version::kCurrent) {
        std::wcerr << L"FAILED: generated=" << version::kCurrent
                   << L", resource=" << fileVersion << L'\n';
        return 1;
    }
    std::wcout << L"Version consistency passed: " << version::kCurrent << L'\n';
    return 0;
}
