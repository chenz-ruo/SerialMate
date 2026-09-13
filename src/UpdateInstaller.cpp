#include "UpdateInstaller.h"

#include "Product.h"
#include "Utilities.h"

#include <shellapi.h>
#include <tlhelp32.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace {

class UpdateMutex final {
public:
    UpdateMutex() {
        handle_ = CreateMutexW(nullptr, FALSE, product::kUpdateMutex);
        if (handle_) acquired_ = WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0;
    }
    ~UpdateMutex() {
        if (acquired_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }
    bool Acquired() const { return acquired_; }

private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
};

std::wstring ModulePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return path;
}

std::wstring Quote(const std::wstring& value) {
    std::wstring result = L"\"";
    unsigned backslashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(ch);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool ReadPayload(const std::wstring& path, std::uint64_t maximum,
                 std::vector<std::uint8_t>& bytes) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto length = input.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > maximum) return false;
    bytes.resize(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), length);
    return static_cast<bool>(input) || bytes.empty();
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
    std::error_code error;
    const auto a = std::filesystem::weakly_canonical(left, error).wstring();
    error.clear();
    const auto b = std::filesystem::weakly_canonical(right, error).wstring();
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool OtherTargetInstanceExists(const std::wstring& target) {
    const DWORD current = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == current) continue;
            const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            std::wstring path(32768, L'\0');
            DWORD count = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process, 0, path.data(), &count)) {
                path.resize(count);
                found = SamePath(path, target);
            }
            CloseHandle(process);
            if (found) break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

int ApplyUpdate(DWORD parentId, const UpdateManifest& manifest,
                const std::wstring& source, const std::wstring& target) {
    UpdateMutex updateMutex;
    if (!updateMutex.Acquired()) {
        MessageBoxW(nullptr, L"另一个实例正在执行更新，本次更新已取消。",
                    L"更新进行中", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    if (SamePath(source, target)) return 2;
    if (const HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentId)) {
        const DWORD wait = WaitForSingleObject(parent, 30000);
        CloseHandle(parent);
        if (wait != WAIT_OBJECT_0) return 3;
    }
    if (OtherTargetInstanceExists(target)) {
        MessageBoxW(nullptr, L"仍有另一个串口助手实例正在运行，已取消更新。请关闭所有实例后重试。",
                    L"更新未安装", MB_OK | MB_ICONWARNING);
        return 4;
    }
    std::vector<std::uint8_t> bytes;
    std::wstring error;
    if (!ReadPayload(source, manifest.size, bytes) || !VerifyUpdatePayload(bytes, manifest, error)) {
        MessageBoxW(nullptr, error.empty() ? L"无法读取更新文件。" : error.c_str(),
                    L"更新校验失败", MB_OK | MB_ICONERROR);
        return 5;
    }

    const std::wstring backup = target + L".previous";
    DeleteFileW(backup.c_str());
    if (!MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        MessageBoxW(nullptr, L"无法备份当前版本，更新未安装。", L"更新失败", MB_OK | MB_ICONERROR);
        return 6;
    }
    if (!MoveFileExW(source.c_str(), target.c_str(),
                     MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        MessageBoxW(nullptr, L"无法替换程序文件，已恢复原版本。", L"更新失败", MB_OK | MB_ICONERROR);
        return 7;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = Quote(target);
    if (!CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        std::filesystem::path(target).parent_path().c_str(), &startup, &process)) {
        DeleteFileW(target.c_str());
        MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        MessageBoxW(nullptr, L"新版本无法启动，已恢复原版本。", L"更新失败", MB_OK | MB_ICONERROR);
        return 8;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    MoveFileExW(backup.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    MoveFileExW(ModulePath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 0;
}

}  // namespace

bool LaunchUpdateInstaller(const UpdateDownloadResult& update, std::wstring& error) {
    if (!update.success || update.path.empty()) {
        error = update.error.empty() ? L"更新文件尚未准备完成。" : update.error;
        return false;
    }
    wchar_t temporaryPath[MAX_PATH]{};
    if (GetTempPathW(static_cast<DWORD>(std::size(temporaryPath)), temporaryPath) == 0) {
        error = L"无法获取临时目录。";
        return false;
    }
    const std::wstring helper = std::wstring(temporaryPath) + L"SerialMate-Updater-" +
                                std::to_wstring(GetCurrentProcessId()) + L".exe";
    if (!CopyFileW(ModulePath().c_str(), helper.c_str(), FALSE)) {
        error = L"无法创建临时更新器：" + util::Win32Error(GetLastError());
        return false;
    }
    const std::wstring target = ModulePath();
    std::wstring command = Quote(helper) + L" --apply-update " + std::to_wstring(GetCurrentProcessId()) +
                           L" " + Quote(update.path) + L" " + Quote(target) + L" " +
                           Quote(update.manifest.version) + L" " + Quote(update.manifest.sha256) +
                           L" " + std::to_wstring(update.manifest.size);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, temporaryPath, &startup, &process)) {
        DeleteFileW(helper.c_str());
        error = L"无法启动更新器：" + util::Win32Error(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    error.clear();
    return true;
}

bool TryRunUpdateInstallerMode(int& exitCode) {
    exitCode = 0;
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return false;
    const bool isUpdate = count == 8 && wcscmp(arguments[1], L"--apply-update") == 0;
    if (!isUpdate) {
        LocalFree(arguments);
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long parent = wcstoul(arguments[2], &end, 10);
    UpdateManifest manifest;
    manifest.version = arguments[5];
    manifest.sha256 = arguments[6];
    wchar_t* sizeEnd = nullptr;
    manifest.size = _wcstoui64(arguments[7], &sizeEnd, 10);
    if (!end || *end != L'\0' || parent == 0 || !sizeEnd || *sizeEnd != L'\0' || manifest.size == 0) {
        exitCode = 1;
    } else {
        exitCode = ApplyUpdate(static_cast<DWORD>(parent), manifest, arguments[3], arguments[4]);
    }
    LocalFree(arguments);
    return true;
}
