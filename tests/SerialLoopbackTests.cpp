#include "../src/SerialPort.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

std::atomic<DWORD> lastSerialError{ERROR_SUCCESS};

class Receiver {
public:
    void Add(std::vector<std::uint8_t> bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.insert(data_.end(), bytes.begin(), bytes.end());
        condition_.notify_all();
    }
    void Clear() { std::lock_guard<std::mutex> lock(mutex_); data_.clear(); }
    bool WaitFor(const std::vector<std::uint8_t>& expected, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] {
            return std::search(data_.begin(), data_.end(), expected.begin(), expected.end()) != data_.end();
        });
    }
    bool WaitForSize(std::size_t size, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] { return data_.size() >= size; });
    }
    std::vector<std::uint8_t> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_;
    }
    std::size_t Size() const { std::lock_guard<std::mutex> lock(mutex_); return data_.size(); }
private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::uint8_t> data_;
};

bool Open(SerialPort& serial, const std::wstring& port, Receiver& receiver, std::wstring& error) {
    lastSerialError.store(ERROR_SUCCESS);
    SerialConfig config{};
    config.port = port;
    config.baudRate = CBR_115200;
    return serial.Open(config,
        [&receiver](std::vector<std::uint8_t> bytes) { receiver.Add(std::move(bytes)); },
        [&error](DWORD code, std::wstring message) {
            lastSerialError.store(code);
            error = message + L" (" + std::to_wstring(code) + L")";
        },
        error);
}

}

int wmain(int argc, wchar_t** argv) {
    const std::wstring port = argc > 1 ? argv[1] : L"COM7";
    const int payloadKiB = argc > 2 ? std::clamp(_wtoi(argv[2]), 1, 4096) : 256;
    std::wcout << L"Testing loopback port " << port << L" at 115200 baud...\n";
    SerialPort serial;
    Receiver receiver;
    std::wstring error;

    for (int iteration = 0; iteration < 10; ++iteration) {
        if (!Open(serial, port, receiver, error)) {
            std::wcerr << L"Open/close iteration " << iteration << L" failed: " << error << L'\n';
            return 2;
        }
        serial.Close();
    }
    std::wcout << L"PASS: 10 repeated open/close cycles\n";

    if (!Open(serial, port, receiver, error)) {
        std::wcerr << L"Open failed: " << error << L'\n';
        return 2;
    }

    SerialPort competing;
    Receiver competingReceiver;
    std::wstring competingError;
    if (Open(competing, port, competingReceiver, competingError)) {
        std::wcerr << L"A second process unexpectedly opened the occupied port\n";
        competing.Close();
        return 7;
    }
    std::wcout << L"PASS: occupied port is rejected without disturbing active connection\n";

    std::vector<std::uint8_t> oversized(4 * 1024 * 1024 + 1, 0x5a);
    if (serial.Send(std::move(oversized))) {
        std::wcerr << L"Oversized send queue item was not rejected\n";
        return 8;
    }
    std::wcout << L"PASS: 4 MiB send queue bound is enforced\n";

    const std::vector<std::uint8_t> ascii{'S','e','r','i','a','l','M','a','t','e','-','A','S','C','I','I','\r','\n'};
    receiver.Clear();
    if (!serial.Send(ascii) || !receiver.WaitFor(ascii, std::chrono::seconds(3))) {
        std::wcerr << L"ASCII loopback failed; received " << receiver.Size() << L" bytes\n";
        return 3;
    }
    std::wcout << L"PASS: ASCII + CRLF loopback\n";

    const std::vector<std::uint8_t> binary{0x00, 0x01, 0x0a, 0x0d, 0x7f, 0x80, 0xfe, 0xff};
    receiver.Clear();
    if (!serial.Send(binary) || !receiver.WaitFor(binary, std::chrono::seconds(3))) {
        std::wcerr << L"Binary/HEX loopback failed; received " << receiver.Size()
                   << L" bytes, open=" << serial.IsOpen() << L", code="
                   << lastSerialError.load() << L", error=" << error << L'\n';
        return 4;
    }
    std::wcout << L"PASS: binary/HEX loopback\n";

    const std::vector<std::uint8_t> utf8{0xe4,0xbd,0xa0,0xe5,0x9c,0xa8,0xe5,0xb9,0xb2,0xe5,0x98,0x9b};
    receiver.Clear();
    if (!serial.Send(utf8) || !receiver.WaitFor(utf8, std::chrono::seconds(3))) {
        std::wcerr << L"UTF-8 byte stream loopback failed\n";
        return 9;
    }
    const std::vector<std::uint8_t> gbk{0xc4,0xe3,0xd4,0xda,0xb8,0xc9,0xc2,0xef};
    receiver.Clear();
    if (!serial.Send(gbk) || !receiver.WaitFor(gbk, std::chrono::seconds(3))) {
        std::wcerr << L"GBK byte stream loopback failed\n";
        return 10;
    }
    std::wcout << L"PASS: UTF-8 and GBK byte streams\n";

    long long latencyTotal = 0;
    long long latencyMaximum = 0;
    constexpr int latencySamples = 20;
    for (int sample = 0; sample < latencySamples; ++sample) {
        const std::vector<std::uint8_t> marker{0xa5, static_cast<std::uint8_t>(sample), 0x5a};
        receiver.Clear();
        const auto started = std::chrono::steady_clock::now();
        if (!serial.Send(marker) || !receiver.WaitFor(marker, std::chrono::seconds(2))) {
            std::wcerr << L"Low-volume latency sample timed out\n";
            return 11;
        }
        const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        latencyTotal += latency;
        latencyMaximum = std::max(latencyMaximum, latency);
    }
    std::wcout << L"PASS: low-volume latency samples=" << latencySamples
               << L", averageMs=" << (latencyTotal / latencySamples)
               << L", maximumMs=" << latencyMaximum << L'\n';

    std::vector<std::uint8_t> block(1024);
    for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<std::uint8_t>(i & 0xff);
    receiver.Clear();
    const int blocks = payloadKiB;
    for (int i = 0; i < blocks; ++i) {
        if (!serial.Send(block)) {
            std::wcerr << L"Sustained send queue rejected block " << i << L'\n';
            return 5;
        }
    }
    const std::size_t expectedBytes = block.size() * static_cast<std::size_t>(blocks);
    const auto timeout = std::chrono::seconds(std::max(35, payloadKiB / 8 + 30));
    if (!receiver.WaitForSize(expectedBytes, timeout)) {
        std::wcerr << L"Sustained loopback timed out; expected " << expectedBytes << L", received " << receiver.Size() << L'\n';
        std::wcerr << L"Serial state: open=" << serial.IsOpen() << L", pending="
                   << serial.PendingBytes() << L", code=" << lastSerialError.load() << L'\n';
        return 6;
    }
    const auto received = receiver.Snapshot();
    bool exact = received.size() == expectedBytes;
    for (std::size_t offset = 0; exact && offset < received.size(); ++offset)
        exact = received[offset] == block[offset % block.size()];
    if (!exact) {
        std::wcerr << L"Sustained loopback content mismatch; expected " << expectedBytes
                   << L", received " << received.size() << L" bytes\n";
        return 12;
    }
    std::wcout << L"PASS: sustained " << payloadKiB << L" KiB byte-exact loopback\n";
    serial.Close();
    for (const std::size_t tailSize : {std::size_t(1), std::size_t(10), std::size_t(100), std::size_t(1024)}) {
        if (!Open(serial, port, receiver, error)) {
            std::wcerr << L"Close-tail open failed for " << tailSize << L" bytes\n";
            return 13;
        }
        receiver.Clear();
        std::vector<std::uint8_t> tail(tailSize, 0x42);
        if (!serial.Send(tail)) {
            std::wcerr << L"Close-tail send failed for " << tailSize << L" bytes\n";
            return 14;
        }
        const auto closeResult = serial.Close();
        if (closeResult != SerialPort::CloseResult::Clean ||
            !receiver.WaitForSize(tailSize, std::chrono::seconds(3))) {
            std::wcerr << L"Close-tail flush failed for " << tailSize << L" bytes, received "
                       << receiver.Size() << L"\n";
            return 15;
        }
    }
    std::wcout << L"PASS: immediate close flushes 1/10/100/1024-byte tails\n";
    std::wcout << L"All COM loopback tests passed.\n";
    return 0;
}

