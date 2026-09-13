#include "../src/RawRxWriter.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int wmain() {
    const auto path = std::filesystem::temp_directory_path() /
        (L"SerialMate-RawRxWriterTests-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    const std::vector<std::uint8_t> first{0x00, 0x80, 0xff, 0xe4, 0xbd, 0xa0};
    const std::vector<std::uint8_t> second{0x0d, 0x0a, 0xc4, 0xe3};
    RawRxWriter writer;
    if (!writer.Start(path.wstring())) return 1;
    writer.Write(first);
    writer.Write(second);
    writer.Stop();
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> fileBytes((std::istreambuf_iterator<char>(input)), {});
    std::vector<std::uint8_t> actual;
    actual.reserve(fileBytes.size());
    for (const char value : fileBytes) actual.push_back(static_cast<std::uint8_t>(value));
    std::vector<std::uint8_t> expected = first;
    expected.insert(expected.end(), second.begin(), second.end());
    std::filesystem::remove(path, ignored);
    if (actual != expected) {
        std::wcerr << L"Raw RX file does not exactly match input bytes.\n";
        return 2;
    }
    std::wcout << L"PASS: raw RX writer preserved " << actual.size() << L" bytes exactly.\n";
    return 0;
}
