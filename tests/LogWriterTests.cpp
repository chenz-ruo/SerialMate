#include "../src/LogWriter.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {
int failures = 0;
void Check(bool condition, const wchar_t* name) {
    if (!condition) {
        std::wcerr << L"FAILED: " << name << L'\n';
        ++failures;
    }
}
}

int wmain() {
    const auto path = std::filesystem::temp_directory_path() /
        (L"SerialMate-LogWriterTests-" + std::to_wstring(GetCurrentProcessId()) + L".log");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    logdetail::StructuredRxAssembler assembler;
    constexpr std::size_t kChunkSize = 64 * 1024;
    constexpr std::size_t kChunkCount = 100 * 1024 * 1024 / kChunkSize;
    std::size_t emittedBytes = 0;
    std::size_t maximumPending = 0;
    for (std::size_t index = 0; index < kChunkCount; ++index) {
        comm::Record stress;
        stress.timestamp = L"T" + std::to_wstring(index);
        stress.direction = comm::Direction::Rx;
        stress.rawBytes.assign(kChunkSize, static_cast<std::uint8_t>('A'));
        if (index != 0) stress.rawBytes.front() = 0xa0;
        stress.rawBytes[kChunkSize - 2] = 0xe4;
        stress.rawBytes[kChunkSize - 1] = 0xbd;
        for (const auto& chunk : assembler.Feed(stress, textcodec::TextEncoding::Utf8))
            emittedBytes += chunk.record.rawBytes.size();
        maximumPending = std::max(maximumPending, assembler.PendingSize());
    }
    for (const auto& chunk : assembler.Flush()) emittedBytes += chunk.record.rawBytes.size();
    Check(emittedBytes == 100ULL * 1024ULL * 1024ULL, L"100 MiB RX assembler byte accounting");
    Check(maximumPending <= 3 && assembler.PendingSize() == 0,
          L"100 MiB pathological split keeps only bounded UTF-8 suffix");

    logdetail::StructuredRxAssembler switchAssembler;
    comm::Record utf8Half;
    utf8Half.direction = comm::Direction::Rx;
    utf8Half.timestamp = L"T1";
    utf8Half.rawBytes = {0xe4, 0xbd};
    Check(switchAssembler.Feed(utf8Half, textcodec::TextEncoding::Utf8).empty(),
          L"UTF-8 suffix remains pending");
    comm::Record gbkRecord;
    gbkRecord.direction = comm::Direction::Rx;
    gbkRecord.timestamp = L"T2";
    gbkRecord.rawBytes = {0xc4, 0xe3};
    const auto switched = switchAssembler.Feed(gbkRecord, textcodec::TextEncoding::Gbk);
    Check(switched.size() == 2 && switched[0].encoding == textcodec::TextEncoding::Utf8 &&
              switched[0].record.timestamp == L"T1" &&
              switched[1].encoding == textcodec::TextEncoding::Gbk,
          L"encoding switch flushes pending bytes using original encoding and timestamp");

    LogWriter writer;
    Check(writer.Start(path.wstring()), L"start log writer");
    comm::Record record;
    record.timestamp = L"T";
    record.direction = comm::Direction::Rx;
    record.rawBytes = {0xc4, 0xe3};
    writer.WriteRecord(record, textcodec::TextEncoding::Gbk);
    writer.Write(std::string(8 * 1024 * 1024 + 1, 'X'));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto overflow = writer.TakeOverflowStatus();
    Check(overflow.records == 1 && overflow.bytes == 8 * 1024 * 1024 + 1,
          L"overflow counters");
    writer.Stop();

    std::ifstream input(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Check(content.find("\xE4\xBD\xA0") != std::string::npos, L"GBK record logged as UTF-8 visible text");
    Check(content.find("[WARNING] Log queue overflow") != std::string::npos,
          L"overflow marker written");
    std::filesystem::remove(path, ignored);

    if (failures == 0) std::wcout << L"All log writer tests passed.\n";
    return failures == 0 ? 0 : 1;
}
