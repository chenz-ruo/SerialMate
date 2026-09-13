#include "../src/UpdateChecker.h"
#include "../src/UpdateInstaller.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::wstring ModulePath() {
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(count);
    return path;
}

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> chars((std::istreambuf_iterator<char>(input)), {});
    std::vector<std::uint8_t> bytes;
    bytes.reserve(chars.size());
    for (const char value : chars) bytes.push_back(static_cast<std::uint8_t>(value));
    return bytes;
}

}  // namespace

int wmain() {
    int updateExitCode = 0;
    if (TryRunUpdateInstallerMode(updateExitCode)) return updateExitCode;
    if (_wcsicmp(std::filesystem::path(ModulePath()).filename().c_str(), L"UpdatedProbe.exe") == 0) return 0;

    const auto directory = std::filesystem::temp_directory_path() /
        (L"SerialMate-UpdateInstallerTests-" + std::to_wstring(GetCurrentProcessId()));
    const auto helper = directory / L"UpdaterProbe.exe";
    const auto payload = directory / L"Payload.exe";
    const auto target = directory / L"UpdatedProbe.exe";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    std::filesystem::copy_file(ModulePath(), helper);
    std::filesystem::copy_file(ModulePath(), payload);
    std::filesystem::copy_file(ModulePath(), target);
    {
        std::ofstream append(payload, std::ios::binary | std::ios::app);
        const char marker[] = "SerialMate-VERIFIED-UPDATE";
        append.write(marker, sizeof(marker));
    }
    const auto expected = ReadBytes(payload);
    const std::wstring hash = Sha256Hex(expected);
    std::wstring command = Quote(helper.wstring()) + L" --apply-update 4294967294 " +
                           Quote(payload.wstring()) + L" " + Quote(target.wstring()) +
                           L" 9.9.9 " + hash + L" " + std::to_wstring(expected.size());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, directory.c_str(), &startup, &process)) {
        std::wcerr << L"FAIL: could not start updater simulation.\n";
        return 1;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    DWORD code = 999;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || code != 0 || ReadBytes(target) != expected) {
        std::wcerr << L"FAIL: updater did not atomically replace the target; wait=" << wait
                   << L", code=" << code << L".\n";
        return 2;
    }
    Sleep(200);
    std::filesystem::remove_all(directory, ignored);
    std::wcout << L"PASS: verified updater helper replaced and launched a simulated EXE.\n";
    return 0;
}
