#include "ExtensionControlIds.h"
#include "Product.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr UINT kQuery = WM_APP + 100;
int checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

struct TestProcess {
    PROCESS_INFORMATION process{};
    HWND window = nullptr;
    ~TestProcess() {
        if (IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 3000) != WAIT_OBJECT_0)
                TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
    }
};

BOOL CALLBACK FindWindowForProcess(HWND candidate, LPARAM parameter) {
    auto& app = *reinterpret_cast<TestProcess*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(candidate, &processId);
    wchar_t className[128]{};
    GetClassNameW(candidate, className, static_cast<int>(std::size(className)));
    if (processId == app.process.dwProcessId && wcscmp(className, product::kMainWindowClass) == 0) {
        app.window = candidate;
        return FALSE;
    }
    return TRUE;
}

void Start(TestProcess& app, const std::filesystem::path& executable) {
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNOACTIVATE;
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                         executable.parent_path().c_str(), &startup, &app.process) != FALSE,
          "cannot start isolated SerialMate");
    WaitForInputIdle(app.process.hProcess, 5000);
    for (int i = 0; i < 100 && !app.window; ++i) {
        EnumWindows(FindWindowForProcess, reinterpret_cast<LPARAM>(&app));
        if (!app.window) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Check(app.window != nullptr, "isolated SerialMate window was not created");
}

std::wstring Text(HWND window) {
    DWORD_PTR length = 0;
    Check(SendMessageTimeoutW(window, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 2000,
                              &length) != 0,
          "WM_GETTEXTLENGTH timed out");
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    DWORD_PTR copied = 0;
    Check(SendMessageTimeoutW(window, WM_GETTEXT, text.size(),
                              reinterpret_cast<LPARAM>(text.data()), SMTO_ABORTIFHUNG, 2000,
                              &copied) != 0,
          "WM_GETTEXT timed out");
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

void Stop(TestProcess& app) {
    PostMessageW(app.window, WM_CLOSE, 0, 0);
    app.window = nullptr;
    Check(WaitForSingleObject(app.process.hProcess, 5000) == WAIT_OBJECT_0,
          "isolated SerialMate did not exit normally");
    DWORD exitCode = 1;
    Check(GetExitCodeProcess(app.process.hProcess, &exitCode) && exitCode == 0,
          "isolated SerialMate exit code was not zero");
}

void SetTestValue(HWND window, ULONG_PTR key, const std::wstring& value) {
    COPYDATASTRUCT copy{};
    copy.dwData = key;
    copy.cbData = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    copy.lpData = const_cast<wchar_t*>(value.c_str());
    Check(SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy)) != FALSE,
          "test value was rejected");
}

std::vector<char> ReadBytes(const std::filesystem::path& path) {
    FILE* file = nullptr;
    _wfopen_s(&file, path.c_str(), L"rb");
    if (!file) return {};
    std::vector<char> bytes;
    char buffer[4096];
    while (const std::size_t count = fread(buffer, 1, sizeof(buffer), file))
        bytes.insert(bytes.end(), buffer, buffer + count);
    fclose(file);
    return bytes;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto directory = std::filesystem::temp_directory_path() /
        (L"SerialMate-CustomDataPersistence-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
    std::filesystem::create_directories(directory);
    const auto executable = directory / L"SerialMate.exe";
    try {
        Check(CopyFileW(std::filesystem::absolute(argv[1]).c_str(), executable.c_str(), FALSE) != FALSE,
              "cannot copy test executable");
        TestProcess first;
        Start(first, executable);
        RECT client{};
        GetClientRect(first.window, &client);
        Check(client.right == SendMessageW(first.window, kQuery, 50, 0),
              "configuration changed the default startup width");
        const std::array<std::pair<int, const wchar_t*>, 3> values{{
            {0, L"AT+版本?"}, {7, L"01 03 00 00 = A"}, {15, L"第十六槽"}}};
        for (const auto& [slot, value] : values) {
            HWND edit = GetDlgItem(first.window, extension::EditFirst + slot);
            Check(edit != nullptr, "custom edit does not exist");
            Check(SendMessageW(edit, EM_GETLIMITTEXT, 0, 0) == 4096, "custom edit limit is not 4096");
            SetTestValue(first.window, 100 + static_cast<ULONG_PTR>(slot), value);
        }
        Stop(first);

        const auto config = directory / L"SerialMate.ini";
        const DWORD attributes = GetFileAttributesW(config.c_str());
        Check(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0,
              "normal exit did not create hidden executable-directory configuration");
        const auto bytes = ReadBytes(config);
        Check(bytes.size() < 3 || static_cast<unsigned char>(bytes[0]) != 0xEF ||
              static_cast<unsigned char>(bytes[1]) != 0xBB || static_cast<unsigned char>(bytes[2]) != 0xBF,
              "persisted UI configuration contains BOM");
        const std::string persisted(bytes.begin(), bytes.end());
        const bool allPersisted = persisted.find(std::string(u8"CustomData01=AT+版本?")) != std::string::npos &&
                                  persisted.find("CustomData08=01 03 00 00 = A") != std::string::npos &&
                                  persisted.find(std::string(u8"CustomData16=第十六槽")) != std::string::npos;
        if (!allPersisted) std::cerr << "Persisted configuration:\n" << persisted << '\n';
        Check(allPersisted, "normal exit did not persist all edited slot values");

        TestProcess second;
        Start(second, executable);
        for (const auto& [slot, value] : values) {
            const auto actual = Text(GetDlgItem(second.window, extension::EditFirst + slot));
            if (actual != value)
                std::wcerr << L"Slot " << slot + 1 << L" expected=[" << value
                           << L"] actual=[" << actual << L"]\n";
            Check(actual == value, "custom value did not survive restart");
        }
        Stop(second);
        std::filesystem::remove_all(directory, cleanupError);
        std::cout << "Custom data persistence UI test passed. Checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << " (check " << checks << ")\n";
        std::filesystem::remove_all(directory, cleanupError);
        return 1;
    }
}
