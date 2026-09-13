#include "SerialPort.h"

#include "Utilities.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <utility>

namespace {

constexpr DWORD kWorkerShutdownTimeoutMs = 2500;
constexpr DWORD kForcedShutdownTimeoutMs = 1000;
constexpr auto kWriteTimeout = std::chrono::milliseconds(2000);
constexpr auto kReceiveBatchDelay = std::chrono::milliseconds(30);

struct SignalEventOnExit final {
    HANDLE event = nullptr;
    ~SignalEventOnExit() { if (event) SetEvent(event); }
};

bool DrainOverlapped(HANDLE handle, OVERLAPPED& overlapped, bool& pending) {
    if (!pending) return true;
    CancelIoEx(handle, &overlapped);
    for (;;) {
        DWORD transferred = 0;
        if (GetOverlappedResult(handle, &overlapped, &transferred, FALSE)) {
            pending = false;
            return true;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_OPERATION_ABORTED || error == ERROR_INVALID_HANDLE ||
            error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_GEN_FAILURE) {
            pending = false;
            return true;
        }
        if (error != ERROR_IO_INCOMPLETE) {
            pending = false;
            return false;
        }
        // Close() has a bounded UI wait. If a broken driver never completes
        // cancellation, this shared-state worker remains alive with the
        // OVERLAPPED and its event instead of destroying either prematurely.
        WaitForSingleObject(overlapped.hEvent, INFINITE);
    }
}

} // namespace

struct SerialPort::State final {
    State() {
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        writeReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        workerExitedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~State() {
        const HANDLE active = handle.exchange(INVALID_HANDLE_VALUE);
        if (active != INVALID_HANDLE_VALUE) CloseHandle(active);
        if (workerExitedEvent) CloseHandle(workerExitedEvent);
        if (writeReadyEvent) CloseHandle(writeReadyEvent);
        if (stopEvent) CloseHandle(stopEvent);
    }

    bool Valid() const noexcept {
        return stopEvent && writeReadyEvent && workerExitedEvent;
    }

    std::atomic<HANDLE> handle{INVALID_HANDLE_VALUE};
    HANDLE stopEvent = nullptr;
    HANDLE writeReadyEvent = nullptr;
    HANDLE workerExitedEvent = nullptr;
    std::atomic_bool open{false};
    std::atomic_bool stopping{false};
    std::atomic_bool callbacksEnabled{true};
    std::atomic_bool errorReported{false};
    mutable std::mutex writeMutex;
    std::deque<std::vector<std::uint8_t>> writeQueue;
    std::size_t pendingBytes = 0;
    static constexpr std::size_t kMaximumPendingBytes = 4 * 1024 * 1024;
    std::mutex callbackMutex;
    DataCallback onData;
    ErrorCallback onError;
};

SerialPort::~SerialPort() {
    Close();
}

bool SerialPort::Configure(HANDLE handle, const SerialConfig& config, std::wstring& error) {
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle, &dcb)) {
        error = L"读取串口参数失败：" + util::Win32Error(GetLastError());
        return false;
    }
    dcb.BaudRate = config.baudRate;
    dcb.ByteSize = config.dataBits;
    dcb.StopBits = config.stopBits;
    dcb.Parity = config.parity;
    dcb.fBinary = TRUE;
    dcb.fParity = config.parity != NOPARITY;
    dcb.fOutxCtsFlow = config.rtsCts;
    dcb.fRtsControl = config.rtsCts ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    if (!SetCommState(handle, &dcb)) {
        error = L"设置串口参数失败：" + util::Win32Error(GetLastError());
        return false;
    }

    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1500;
    if (!SetCommTimeouts(handle, &timeouts)) {
        error = L"设置串口超时失败：" + util::Win32Error(GetLastError());
        return false;
    }
    if (!SetCommMask(handle, EV_RXCHAR | EV_ERR | EV_BREAK)) {
        error = L"设置串口事件失败：" + util::Win32Error(GetLastError());
        return false;
    }
    SetupComm(handle, 64 * 1024, 64 * 1024);
    PurgeComm(handle, PURGE_RXABORT | PURGE_TXABORT | PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

bool SerialPort::Open(const SerialConfig& config, DataCallback onData, ErrorCallback onError,
                      std::wstring& error) {
    Close();
    auto state = std::make_shared<State>();
    if (!state->Valid()) {
        error = L"无法创建串口同步对象。";
        return false;
    }

    std::wstring device = config.port;
    if (device.rfind(L"\\\\.\\", 0) != 0) device = L"\\\\.\\" + device;
    const HANDLE handle = CreateFileW(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION) {
            error = L"串口被占用或无法打开：" + config.port;
        } else {
            error = L"打开 " + config.port + L" 失败：" + util::Win32Error(code);
        }
        return false;
    }
    state->handle.store(handle);
    if (!Configure(handle, config, error)) return false;

    state->onData = std::move(onData);
    state->onError = std::move(onError);
    state->open.store(true);
    try {
        std::thread worker(&SerialPort::Worker, state);
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        state_ = std::move(state);
        worker_ = std::move(worker);
    } catch (...) {
        state->open.store(false);
        state->stopping.store(true);
        error = L"无法启动串口通信线程。";
        return false;
    }
    return true;
}

SerialPort::CloseResult SerialPort::Close() {
    std::shared_ptr<State> state;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        state = state_;
        if (!state) {
            if (worker_.joinable()) worker_.join();
            return CloseResult::AlreadyClosed;
        }
    }

    state->stopping.store(true);
    state->open.store(false);
    SetEvent(state->stopEvent);
    HANDLE handle = state->handle.load();
    if (handle != INVALID_HANDLE_VALUE) CancelIoEx(handle, nullptr);

    CloseResult result = CloseResult::Clean;
    DWORD wait = WaitForSingleObject(state->workerExitedEvent, kWorkerShutdownTimeoutMs);
    if (wait != WAIT_OBJECT_0) {
        // A bad USB driver may ignore CancelIoEx. Closing the file handle is
        // the stronger cancellation boundary. The Worker owns only shared
        // State, so a final pathological timeout cannot retain `this`.
        state->callbacksEnabled.store(false);
        handle = state->handle.exchange(INVALID_HANDLE_VALUE);
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        result = CloseResult::ForcedHandleClose;
        wait = WaitForSingleObject(state->workerExitedEvent, kForcedShutdownTimeoutMs);
    }

    {
        std::lock_guard<std::mutex> lock(state->callbackMutex);
        state->onData = {};
        state->onError = {};
    }
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (wait == WAIT_OBJECT_0) {
            if (worker_.joinable()) worker_.join();
        } else {
            if (worker_.joinable()) worker_.detach();
            result = CloseResult::WorkerDetached;
        }
        if (state_ == state) state_.reset();
    }

    handle = state->handle.exchange(INVALID_HANDLE_VALUE);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    {
        std::lock_guard<std::mutex> lock(state->writeMutex);
        state->writeQueue.clear();
        state->pendingBytes = 0;
    }
    return result;
}

bool SerialPort::Send(std::vector<std::uint8_t> data) {
    std::shared_ptr<State> state;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        state = state_;
    }
    if (!state || !state->open.load() || state->stopping.load() || data.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(state->writeMutex);
        if (!state->open.load() || state->pendingBytes + data.size() > State::kMaximumPendingBytes)
            return false;
        state->pendingBytes += data.size();
        state->writeQueue.push_back(std::move(data));
    }
    SetEvent(state->writeReadyEvent);
    return true;
}

bool SerialPort::IsOpen() const noexcept {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return state_ && state_->open.load();
}

std::size_t SerialPort::PendingBytes() const {
    std::shared_ptr<State> state;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        state = state_;
    }
    if (!state) return 0;
    std::lock_guard<std::mutex> lock(state->writeMutex);
    return state->pendingBytes;
}

void SerialPort::ReportFatal(const std::shared_ptr<State>& state, DWORD code,
                             const wchar_t* context) {
    state->open.store(false);
    if (state->stopping.load() || state->errorReported.exchange(true)) return;
    ErrorCallback callback;
    {
        std::lock_guard<std::mutex> lock(state->callbackMutex);
        if (state->callbacksEnabled.load()) callback = state->onError;
    }
    if (callback) callback(code, std::wstring(context) + L"：" + util::Win32Error(code));
}

void SerialPort::Worker(std::shared_ptr<State> state) {
    SignalEventOnExit exited{state->workerExitedEvent};
    const HANDLE handle = state->handle.load();
    if (handle == INVALID_HANDLE_VALUE) return;

    OVERLAPPED commOverlapped{};
    OVERLAPPED readOverlapped{};
    OVERLAPPED writeOverlapped{};
    commOverlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    readOverlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    writeOverlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!commOverlapped.hEvent || !readOverlapped.hEvent || !writeOverlapped.hEvent) {
        ReportFatal(state, GetLastError(), L"创建通信事件失败");
        if (commOverlapped.hEvent) CloseHandle(commOverlapped.hEvent);
        if (readOverlapped.hEvent) CloseHandle(readOverlapped.hEvent);
        if (writeOverlapped.hEvent) CloseHandle(writeOverlapped.hEvent);
        return;
    }

    bool commPending = false;
    bool readPending = false;
    bool writePending = false;
    DWORD commMask = 0;
    std::vector<std::uint8_t> readBuffer(8192);
    std::vector<std::uint8_t> writePacket;
    std::vector<std::uint8_t> deliveryBuffer;
    deliveryBuffer.reserve(16 * 1024);
    auto lastDelivery = std::chrono::steady_clock::now();
    auto writeStarted = lastDelivery;

    const auto collectReceived = [&](DWORD received) {
        if (received == 0) return;
        if (deliveryBuffer.empty()) lastDelivery = std::chrono::steady_clock::now();
        deliveryBuffer.insert(deliveryBuffer.end(), readBuffer.begin(),
                              readBuffer.begin() + received);
    };
    const auto deliverReceived = [&] {
        if (deliveryBuffer.empty()) return;
        DataCallback callback;
        {
            std::lock_guard<std::mutex> lock(state->callbackMutex);
            if (state->callbacksEnabled.load()) callback = state->onData;
        }
        if (callback) callback(std::move(deliveryBuffer));
        deliveryBuffer.clear();
        deliveryBuffer.reserve(16 * 1024);
        lastDelivery = std::chrono::steady_clock::now();
    };
    const auto completeWriteAccounting = [&] {
        std::lock_guard<std::mutex> lock(state->writeMutex);
        state->pendingBytes -= std::min(state->pendingBytes, writePacket.size());
        writePacket.clear();
    };
    const auto startRead = [&](bool& activity) -> bool {
        activity = false;
        if (readPending || state->stopping.load()) return true;
        DWORD errors = 0;
        COMSTAT status{};
        if (!ClearCommError(handle, &errors, &status)) return false;
        if (status.cbInQue == 0) return true;
        ResetEvent(readOverlapped.hEvent);
        DWORD received = 0;
        const DWORD requested = std::min<DWORD>(static_cast<DWORD>(readBuffer.size()), status.cbInQue);
        if (ReadFile(handle, readBuffer.data(), requested, &received, &readOverlapped)) {
            ResetEvent(readOverlapped.hEvent);
            collectReceived(received);
            activity = received > 0;
            return true;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            readPending = true;
            activity = true;
            return true;
        }
        SetLastError(error);
        return false;
    };

    while (!state->stopping.load() && state->open.load()) {
        bool readFailed = false;
        while (!readPending) {
            bool readActivity = false;
            if (!startRead(readActivity)) {
                ReportFatal(state, GetLastError(), L"串口接收失败");
                readFailed = true;
                break;
            }
            if (!readActivity) break;
        }
        if (readFailed) break;

        bool commCompletedImmediately = false;
        if (!commPending && !readPending) {
            ResetEvent(commOverlapped.hEvent);
            commMask = 0;
            if (WaitCommEvent(handle, &commMask, &commOverlapped)) {
                ResetEvent(commOverlapped.hEvent);
                commCompletedImmediately = true;
            } else {
                const DWORD error = GetLastError();
                if (error == ERROR_IO_PENDING) commPending = true;
                else {
                    ReportFatal(state, error, L"等待串口事件失败");
                    break;
                }
            }
        }

        if (!writePending && writePacket.empty()) {
            {
                std::lock_guard<std::mutex> lock(state->writeMutex);
                if (!state->writeQueue.empty()) {
                    writePacket = std::move(state->writeQueue.front());
                    state->writeQueue.pop_front();
                }
                if (state->writeQueue.empty()) ResetEvent(state->writeReadyEvent);
            }
            if (!writePacket.empty()) {
                ResetEvent(writeOverlapped.hEvent);
                DWORD written = 0;
                if (WriteFile(handle, writePacket.data(), static_cast<DWORD>(writePacket.size()),
                              &written, &writeOverlapped)) {
                    ResetEvent(writeOverlapped.hEvent);
                    completeWriteAccounting();
                } else {
                    const DWORD error = GetLastError();
                    if (error == ERROR_IO_PENDING) {
                        writePending = true;
                        writeStarted = std::chrono::steady_clock::now();
                    } else {
                        completeWriteAccounting();
                        ReportFatal(state, error, L"串口发送失败");
                        break;
                    }
                }
            }
        }

        // A synchronous WaitCommEvent completion has no outstanding event to
        // wait on. Poll the other events once, then immediately re-arm it.
        DWORD timeout = commCompletedImmediately ? 0 : INFINITE;
        const auto now = std::chrono::steady_clock::now();
        if (!deliveryBuffer.empty()) {
            const auto elapsed = now - lastDelivery;
            timeout = elapsed >= kReceiveBatchDelay ? 0 : static_cast<DWORD>(
                std::chrono::duration_cast<std::chrono::milliseconds>(kReceiveBatchDelay - elapsed).count());
        }
        if (writePending) {
            const auto elapsed = now - writeStarted;
            const DWORD writeRemaining = elapsed >= kWriteTimeout ? 0 : static_cast<DWORD>(
                std::chrono::duration_cast<std::chrono::milliseconds>(kWriteTimeout - elapsed).count());
            timeout = timeout == INFINITE ? writeRemaining : std::min(timeout, writeRemaining);
        }

        HANDLE waits[] = {state->stopEvent, state->writeReadyEvent, commOverlapped.hEvent,
                          readOverlapped.hEvent, writeOverlapped.hEvent};
        // The manual-reset queue event is only a wake-up for starting a new
        // write. While one is pending it must not starve the lower-priority
        // OVERLAPPED completion event in WaitForMultipleObjects.
        if (writePending) ResetEvent(state->writeReadyEvent);
        const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(std::size(waits)), waits,
                                                  FALSE, timeout);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            ReportFatal(state, GetLastError(), L"等待串口 I/O 失败");
            break;
        }

        if (wait == WAIT_OBJECT_0 + 2 && commPending) {
            DWORD ignored = 0;
            if (GetOverlappedResult(handle, &commOverlapped, &ignored, FALSE)) {
                commPending = false;
                ResetEvent(commOverlapped.hEvent);
                if (commMask & (EV_ERR | EV_BREAK)) {
                    DWORD errors = 0;
                    COMSTAT status{};
                    if (!ClearCommError(handle, &errors, &status)) {
                        ReportFatal(state, GetLastError(), L"读取串口状态失败");
                        break;
                    }
                }
            } else {
                const DWORD error = GetLastError();
                commPending = false;
                if (!state->stopping.load()) ReportFatal(state, error, L"等待串口事件失败");
                break;
            }
        }

        if (wait == WAIT_OBJECT_0 + 3 && readPending) {
            DWORD received = 0;
            if (GetOverlappedResult(handle, &readOverlapped, &received, FALSE)) {
                readPending = false;
                ResetEvent(readOverlapped.hEvent);
                collectReceived(received);
            } else {
                const DWORD error = GetLastError();
                readPending = false;
                if (!state->stopping.load()) ReportFatal(state, error, L"串口接收失败");
                break;
            }
        }

        if (wait == WAIT_OBJECT_0 + 4 && writePending) {
            DWORD written = 0;
            const bool ok = GetOverlappedResult(handle, &writeOverlapped, &written, FALSE) != FALSE;
            const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
            writePending = false;
            ResetEvent(writeOverlapped.hEvent);
            completeWriteAccounting();
            if (!ok && !state->stopping.load()) {
                ReportFatal(state, error, L"串口发送失败");
                break;
            }
        }

        const auto afterWait = std::chrono::steady_clock::now();
        if (writePending && afterWait - writeStarted >= kWriteTimeout) {
            DrainOverlapped(handle, writeOverlapped, writePending);
            completeWriteAccounting();
            if (!state->stopping.load()) ReportFatal(state, ERROR_SEM_TIMEOUT, L"串口发送超时");
            break;
        }
        if (!deliveryBuffer.empty() &&
            (deliveryBuffer.size() >= readBuffer.size() || afterWait - lastDelivery >= kReceiveBatchDelay)) {
            deliverReceived();
        }
    }

    state->stopping.store(true);
    state->open.store(false);
    // A write started by the main loop may still be pending. It must reach a
    // terminal state before the close-tail loop reuses the OVERLAPPED storage.
    if (writePending) {
        DrainOverlapped(handle, writeOverlapped, writePending);
        completeWriteAccounting();
    }
    // Stop was requested while a small packet was still queued. Complete
    // queued writes before the final receive drain so loopback devices do not
    // lose the last byte on an immediate close.
    bool hadWriteActivity = !writePacket.empty();
    const auto writeDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < writeDeadline) {
        if (writePacket.empty()) {
            std::lock_guard<std::mutex> lock(state->writeMutex);
            if (!state->writeQueue.empty()) {
                writePacket = std::move(state->writeQueue.front());
                hadWriteActivity = true;
            }
            if (!state->writeQueue.empty()) state->writeQueue.pop_front();
        }
        if (writePacket.empty()) break;
        ResetEvent(writeOverlapped.hEvent);
        DWORD written = 0;
        if (WriteFile(handle, writePacket.data(), static_cast<DWORD>(writePacket.size()), &written, &writeOverlapped)) {
            ResetEvent(writeOverlapped.hEvent);
            completeWriteAccounting();
            continue;
        }
        if (GetLastError() != ERROR_IO_PENDING) {
            completeWriteAccounting();
            break;
        }
        writePending = true;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            writeDeadline - std::chrono::steady_clock::now()).count();
        if (WaitForSingleObject(writeOverlapped.hEvent,
                                static_cast<DWORD>(std::clamp<long long>(remaining, 1, 500))) != WAIT_OBJECT_0) {
            CancelIoEx(handle, &writeOverlapped);
            DrainOverlapped(handle, writeOverlapped, writePending);
            completeWriteAccounting();
            break;
        }
        DrainOverlapped(handle, writeOverlapped, writePending);
        completeWriteAccounting();
    }
    DrainOverlapped(handle, commOverlapped, commPending);
    DrainOverlapped(handle, readOverlapped, readPending);
    if (writePending) {
        DrainOverlapped(handle, writeOverlapped, writePending);
        completeWriteAccounting();
    }
    if (state->callbacksEnabled.load() && handle != INVALID_HANDLE_VALUE) {
        // A loopback/USB UART can deliver the final echoed bytes just after
        // the write completion. Give the driver a short, bounded drain window
        // before signalling worker exit so Clean Close does not lose tails.
        FlushFileBuffers(handle);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(hadWriteActivity ? 500 : 20);
        while (std::chrono::steady_clock::now() < deadline) {
            DWORD errors = 0;
            COMSTAT status{};
            if (!ClearCommError(handle, &errors, &status)) break;
            if (status.cbInQue != 0) {
                const DWORD requested = std::min<DWORD>(static_cast<DWORD>(readBuffer.size()), status.cbInQue);
                ResetEvent(readOverlapped.hEvent);
                DWORD received = 0;
                if (ReadFile(handle, readBuffer.data(), requested, &received, &readOverlapped)) {
                    ResetEvent(readOverlapped.hEvent);
                    collectReceived(received);
                } else if (GetLastError() == ERROR_IO_PENDING) {
                    readPending = true;
                    const DWORD wait = WaitForSingleObject(readOverlapped.hEvent, 20);
                    if (wait == WAIT_OBJECT_0) {
                        if (GetOverlappedResult(handle, &readOverlapped, &received, FALSE)) {
                            readPending = false;
                            ResetEvent(readOverlapped.hEvent);
                            collectReceived(received);
                        } else {
                            const DWORD completionError = GetLastError();
                            if (completionError != ERROR_IO_INCOMPLETE) readPending = false;
                            if (readPending) DrainOverlapped(handle, readOverlapped, readPending);
                        }
                    } else {
                        DrainOverlapped(handle, readOverlapped, readPending);
                        break;
                    }
                } else {
                    break;
                }
            } else {
                Sleep(2);
            }
        }
        DrainOverlapped(handle, readOverlapped, readPending);
        deliverReceived();
    } else {
        deliveryBuffer.clear();
    }
    // Every issued OVERLAPPED operation has reached a terminal completion
    // state before its event is released. CancelIoEx alone is not completion.
    DrainOverlapped(handle, commOverlapped, commPending);
    DrainOverlapped(handle, readOverlapped, readPending);
    if (writePending) {
        DrainOverlapped(handle, writeOverlapped, writePending);
        completeWriteAccounting();
    }
    CloseHandle(commOverlapped.hEvent);
    CloseHandle(readOverlapped.hEvent);
    CloseHandle(writeOverlapped.hEvent);
}
