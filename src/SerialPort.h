#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SerialConfig {
    std::wstring port;
    DWORD baudRate = CBR_115200;
    BYTE dataBits = 8;
    BYTE stopBits = ONESTOPBIT;
    BYTE parity = NOPARITY;
    bool rtsCts = false;
};

class SerialPort final {
public:
    using DataCallback = std::function<void(std::vector<std::uint8_t>)>;
    using ErrorCallback = std::function<void(DWORD, std::wstring)>;

    enum class CloseResult {
        AlreadyClosed,
        Clean,
        ForcedHandleClose,
        WorkerDetached,
    };

    SerialPort() = default;
    ~SerialPort();
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    bool Open(const SerialConfig& config, DataCallback onData, ErrorCallback onError,
              std::wstring& error);
    CloseResult Close();
    bool Send(std::vector<std::uint8_t> data);
    bool IsOpen() const noexcept;
    std::size_t PendingBytes() const;

private:
    struct State;

    static void Worker(std::shared_ptr<State> state);
    static void ReportFatal(const std::shared_ptr<State>& state, DWORD code,
                            const wchar_t* context);
    static bool Configure(HANDLE handle, const SerialConfig& config, std::wstring& error);

    mutable std::mutex lifecycleMutex_;
    std::shared_ptr<State> state_;
    std::thread worker_;
};
