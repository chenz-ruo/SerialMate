#include "../src/CommRecord.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
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

std::vector<std::uint8_t> Bytes(std::size_t count, std::uint8_t seed = 0x20) {
    std::vector<std::uint8_t> result(count);
    for (std::size_t i = 0; i < count; ++i) result[i] = static_cast<std::uint8_t>(seed + i);
    return result;
}
}

int wmain() {
    using namespace comm;

    const std::size_t lengths[] = {1, 2, 15, 16, 17, 31, 32, 33, 64, 128, 512, 1024};
    RecordBuffer buffer;
    for (std::size_t index = 0; index < sizeof(lengths) / sizeof(lengths[0]); ++index) {
        const auto length = lengths[index];
        const auto direction = (index % 2 == 0) ? Direction::Rx : Direction::Tx;
        const auto& record = buffer.Add(direction, L"2026-09-12 08:25:15.139", Bytes(length));
        Check(record.rawBytes.size() == length, L"原始字节保留");
        Check(record.RowCount() == (length + kBytesPerRow - 1) / kBytesPerRow, L"自适应行数");
        Check(record.firstRow + record.RowCount() <= buffer.BaseRow() + buffer.RowCount(), L"前缀行索引");
    }
    Check(buffer.ByteCount() == 1 + 2 + 15 + 16 + 17 + 31 + 32 + 33 + 64 + 128 + 512 + 1024,
          L"字节计数");

    Record special;
    special.id = 99;
    special.timestamp = L"T";
    special.direction = Direction::Rx;
    special.rawBytes = {0x00, 0x09, 0x0a, 0x0d, 0x1b, 0x20, 0x21, 0x7e, 0x7f, 0x80, 0xff};
    const auto full = FormatRecord(special);
    Check(full.find(L"00 09 0A 0D 1B 20 21 7E 7F 80 FF") != std::wstring::npos, L"特殊字节HEX");
    Check(full.find(L"..... !~...") != std::wstring::npos, L"特殊字节ASCII");
    Check(full.find(L"\r\n") != std::wstring::npos, L"记录换行");

    Record chinese;
    chinese.id = 98;
    chinese.timestamp = L"T";
    chinese.direction = Direction::Rx;
    chinese.rawBytes = {0xe4, 0xb8, 0xad, 0xe6, 0x96, 0x87, 0x0d, 0x0a};
    Check(DisplayText(chinese.rawBytes) == L"中文..", L"UTF-8中文文本解码");
    Check(DisplayTextRange(chinese.rawBytes, 0, 3) == L"中" &&
              DisplayTextRange(chinese.rawBytes, 3, 3) == L"文",
          L"UTF-8中文按字节分行");
    Check(DisplayTextRange(chinese.rawBytes, 0, 2) == L"中" &&
              DisplayTextRange(chinese.rawBytes, 1, 2).empty(),
          L"UTF-8中文跨行不丢失");
    Check(FormatRecord(chinese, CopyFormat::Text).find(L"中文..") != std::wstring::npos,
          L"UTF-8中文文本复制");
    const std::vector<std::uint8_t> gbkChinese{0xd6, 0xd0, 0xce, 0xc4};
    const auto gbkDisplay = DisplayText(gbkChinese, textcodec::TextEncoding::Gbk);
    Check(gbkDisplay == L"中文", L"GBK显示");
    std::vector<std::uint8_t> encoded;
    std::wstring encodeError;
    Check(textcodec::Encode(L"你在干嘛", textcodec::TextEncoding::Utf8, encoded, encodeError) &&
              encoded == std::vector<std::uint8_t>({0xe4,0xbd,0xa0,0xe5,0x9c,0xa8,0xe5,0xb9,0xb2,0xe5,0x98,0x9b}),
          L"UTF-8编码");
    const bool gbkEncode = textcodec::Encode(L"你在干嘛", textcodec::TextEncoding::Gbk, encoded, encodeError);
    const auto expectedGbk = std::vector<std::uint8_t>({0xc4,0xe3,0xd4,0xda,0xb8,0xc9,0xc2,0xef});
    Check(gbkEncode && encoded == expectedGbk, L"GBK编码");
    Check(!textcodec::Encode(L"中文", textcodec::TextEncoding::Ascii, encoded, encodeError) &&
              !encodeError.empty(), L"ASCII拒绝中文");
    textcodec::RxTextDecoder decoder(textcodec::TextEncoding::Utf8);
    Check(decoder.Feed({0xe4, 0xbd}).empty() && decoder.Feed({0xa0}) == L"你", L"UTF-8跨分包解码");
    decoder.Reset(textcodec::TextEncoding::Gbk);
    const auto gbkPart1 = decoder.Feed({0xc4});
    const auto gbkPart2 = decoder.Feed({0xe3});
    Check(gbkPart1.empty() && gbkPart2 == L"你", L"GBK跨分包解码");

    Record longRecord;
    longRecord.id = 100;
    longRecord.timestamp = L"T";
    longRecord.direction = Direction::Tx;
    longRecord.rawBytes = Bytes(33);
    Check(FormatRecord(longRecord).find(L"[T]") != std::wstring::npos, L"时间戳单次出现");
    const auto secondPrefix = FormatRecord(longRecord).find(L"[T]");
    Check(secondPrefix == FormatRecord(longRecord).rfind(L"[T]"), L"长记录时间戳仅首行");

    const auto hex = FormatRecord(special, CopyFormat::Hex);
    const auto text = FormatRecord(special, CopyFormat::Text);
    Check(hex.find(L"00 09 0A 0D") != std::wstring::npos && hex.find(L"|") == std::wstring::npos,
          L"HEX复制");
    Check(text.find(L"..... !~...") != std::wstring::npos && text.find(L"00") == std::wstring::npos,
          L"文本复制");

    RecordBuffer copyBuffer;
    const auto& first = copyBuffer.Add(Direction::Tx, L"A", {0x41, 0x42});
    const auto& second = copyBuffer.Add(Direction::Rx, L"B", {0x43, 0x44});
    copyBuffer.AddMessage(Direction::Notice, L"C", L"notice");
    Check(copyBuffer.Copy(CopyFormat::Hex).find(L"41 42\r\n43 44\r\n") != std::wstring::npos, L"完整HEX复制");
    Check(copyBuffer.Copy(CopyFormat::Text).find(L"AB\r\nCD\r\n") != std::wstring::npos, L"完整文本复制");
    Check(copyBuffer.Copy(CopyFormat::Full).find(L"notice") != std::wstring::npos, L"通知完整复制");
    Check(copyBuffer.Copy(CopyFormat::Full).find(L"NOTICE") == std::wstring::npos, L"系统消息不显示方向标签");
    Check(copyBuffer.Copy(CopyFormat::Full, std::nullopt, false).find(L"[C]") == std::wstring::npos,
          L"关闭时间戳后系统消息不保留时间戳");
    Check(copyBuffer.Copy(CopyFormat::Hex).find(L"notice") == std::wstring::npos, L"通知不进入HEX");
    const auto selected = copyBuffer.Copy(CopyFormat::Full, std::make_pair(first.id, second.id));
    Check(selected.find(L"notice") == std::wstring::npos && selected.find(L"[A]") != std::wstring::npos,
          L"选中完整复制");
    Check(copyBuffer.Copy(CopyFormat::Text, std::make_pair(second.id, second.id)) == L"CD\r\n",
          L"选中文本复制");

    RecordBuffer trimBytes(32, 100);
    trimBytes.Add(Direction::Tx, L"1", Bytes(16));
    const auto& retained = trimBytes.Add(Direction::Tx, L"2", Bytes(17));
    Check(trimBytes.Records().size() == 1 && trimBytes.Records().front().id == retained.id, L"按字节淘汰整条记录");
    Check(trimBytes.BaseRow() == 2 && trimBytes.RowCount() == 3, L"淘汰后绝对行前缀");
    Check(trimBytes.BaseRow(8) == 2 && trimBytes.BaseRow(12) == 2 && trimBytes.BaseRow(16) == 1,
          L"淘汰后自适应行前缀");
    Check(trimBytes.RowAt(0).has_value() && trimBytes.RowAt(1).has_value() &&
              trimBytes.RowAt(2).has_value() && !trimBytes.RowAt(3).has_value(), L"局部行索引");
    Check(trimBytes.RowAt(0)->offset == 0 && trimBytes.RowAt(1)->offset == 8 &&
              trimBytes.RowAt(2)->offset == 16, L"VisualRow偏移");
    Check(trimBytes.RowOf(retained.id).value_or(99) == 0, L"记录局部首行索引");

    RecordBuffer trimRecords(1024, 2);
    const auto& r1 = trimRecords.Add(Direction::Tx, L"1", {1});
    const auto& r2 = trimRecords.Add(Direction::Tx, L"2", {2});
    const auto& r3 = trimRecords.Add(Direction::Tx, L"3", {3});
    Check(trimRecords.Records().size() == 2 && trimRecords.Records().front().id == r2.id, L"按记录数淘汰");
    Check(trimRecords.RowOf(r1.id) == std::nullopt && trimRecords.RowOf(r3.id).has_value(), L"淘汰索引失效");

    bool threw = false;
    try {
        RecordBuffer small(4, 4);
        small.Add(Direction::Tx, L"x", Bytes(5));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    Check(threw, L"超限单条拒绝");
    threw = false;
    try {
        RecordBuffer messages;
        messages.AddMessage(Direction::Notice, L"x", std::wstring(4097, L'N'));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    Check(threw, L"超限通知拒绝");

    RecordBuffer benchmark(4 * 1024 * 1024, 16384);
    const auto benchmarkPacket = Bytes(256);
    for (int index = 0; index < 16384; ++index) {
        benchmark.Add((index & 1) ? Direction::Rx : Direction::Tx, L"benchmark", benchmarkPacket);
    }
    volatile std::uint64_t checksum = 0;
    const auto benchmarkStart = std::chrono::steady_clock::now();
    constexpr std::size_t lookupsPerWidth = 50000;
    for (const std::size_t width : {std::size_t(16), std::size_t(12), std::size_t(8), std::size_t(4)}) {
        const std::size_t rows = benchmark.RowCount(width);
        for (std::size_t index = 0; index < lookupsPerWidth; ++index) {
            const auto row = benchmark.RowAt((index * 7919) % rows, width);
            if (row) checksum += row->record->id + row->offset;
            const auto recordIndex = (index * 3571) % benchmark.Records().size();
            const auto found = benchmark.RowOf(benchmark.Records()[recordIndex].id, width);
            if (found) checksum += *found;
        }
    }
    const auto benchmarkElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - benchmarkStart).count();
    Check(checksum != 0, L"行索引性能校验和");
    Check(benchmarkElapsed < 5000, L"20万次 RowAt/RowOf 基准耗时");
    std::wcout << L"CommRecord benchmark: records=" << benchmark.Records().size()
               << L", bytes=" << benchmark.ByteCount() << L", lookups="
               << (lookupsPerWidth * 2 * 4) << L", elapsedMs=" << benchmarkElapsed << L'\n';

    if (failures == 0) std::wcout << L"All communication record tests passed.\n";
    return failures == 0 ? 0 : 1;
}
