#include "../src/SerialPort.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool condition, const wchar_t* name) {
    if (!condition) {
        std::wcerr << L"FAILED: " << name << L'\n';
        ++failures;
    }
}

bool Open(SerialPort& port, const std::wstring& name, std::atomic_size_t& received) {
    SerialConfig config{};
    config.port = name;
    std::wstring error;
    return port.Open(config,
        [&received](std::vector<std::uint8_t> bytes) { received.fetch_add(bytes.size()); },
        [](DWORD, std::wstring) {}, error);
}
}

int wmain(int argc, wchar_t** argv) {
    SerialPort unopened;
    Check(unopened.Close() == SerialPort::CloseResult::AlreadyClosed, L"close before open");
    Check(unopened.Close() == SerialPort::CloseResult::AlreadyClosed, L"repeated close before open");

    if (argc > 1) {
        const std::wstring portName = argv[1];
        std::atomic_size_t received{0};
        SerialPort port;
        const auto cycleStart = std::chrono::steady_clock::now();
        for (int index = 0; index < 100; ++index) {
            Check(Open(port, portName, received), L"100-cycle open");
            const auto result = port.Close();
            Check(result == SerialPort::CloseResult::Clean, L"100-cycle clean close");
        }
        const auto cycleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cycleStart).count();

        Check(Open(port, portName, received), L"pending read open");
        const auto readCloseStart = std::chrono::steady_clock::now();
        const auto readResult = port.Close();
        const auto readCloseMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - readCloseStart).count();
        Check(readResult == SerialPort::CloseResult::Clean && readCloseMs < 2500,
              L"pending WaitCommEvent close");

        Check(Open(port, portName, received), L"pending write open");
        Check(port.Send(std::vector<std::uint8_t>(4 * 1024 * 1024, 0x5a)), L"queue maximum write");
        const auto writeCloseStart = std::chrono::steady_clock::now();
        const auto writeResult = port.Close();
        const auto writeCloseMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - writeCloseStart).count();
        Check(writeResult != SerialPort::CloseResult::WorkerDetached && writeCloseMs < 3500,
              L"pending write bounded close");

        std::wcout << L"Serial shutdown hardware: cycles=100, cycleMs=" << cycleMs
                   << L", readCloseMs=" << readCloseMs << L", writeCloseMs=" << writeCloseMs
                   << L", received=" << received.load() << L'\n';
    }

    if (failures == 0) std::wcout << L"All serial shutdown tests passed.\n";
    return failures == 0 ? 0 : 1;
}
