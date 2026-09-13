#include "../src/TextCodec.h"

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

void CheckUtf8Split(const std::vector<std::vector<std::uint8_t>>& packets,
                    const std::wstring& expected, const wchar_t* name) {
    textcodec::RxTextDecoder decoder(textcodec::TextEncoding::Utf8);
    std::wstring output;
    for (const auto& packet : packets) output += decoder.Feed(packet);
    output += decoder.Flush();
    Check(output == expected, name);
}

void CheckGbkSplit(const std::vector<std::vector<std::uint8_t>>& packets,
                   const std::wstring& expected, const wchar_t* name) {
    textcodec::RxTextDecoder decoder(textcodec::TextEncoding::Gbk);
    std::wstring output;
    for (const auto& packet : packets) output += decoder.Feed(packet);
    output += decoder.Flush();
    Check(output == expected, name);
}

} // namespace

int wmain() {
    using textcodec::Decode;
    using textcodec::TextEncoding;

    Check(Decode({0x41}, TextEncoding::Utf8) == L"A", L"UTF-8 ASCII");
    Check(Decode({0xc2, 0xa2}, TextEncoding::Utf8) == L"\u00a2", L"UTF-8 2-byte");
    Check(Decode({0xe4, 0xbd, 0xa0}, TextEncoding::Utf8) == L"你", L"UTF-8 3-byte");
    Check(Decode({0xf0, 0x9f, 0x98, 0x80}, TextEncoding::Utf8) == L"\U0001f600", L"UTF-8 4-byte");
    Check(Decode({0x41, 0xe4, 0xbd, 0xa0, 0x42}, TextEncoding::Utf8) == L"A你B",
          L"UTF-8 mixed valid");

    CheckUtf8Split({{0xe4}, {0xbd}, {0xa0}}, L"你", L"UTF-8 split 1+1+1");
    CheckUtf8Split({{0xe4, 0xbd}, {0xa0}}, L"你", L"UTF-8 split 2+1");
    CheckUtf8Split({{0x41, 0xe4}, {0xbd, 0xa0, 0x42}}, L"A你B", L"UTF-8 split A1+2B");
    CheckUtf8Split({{0x41, 0xe4, 0xbd}, {0xa0, 0x42}}, L"A你B", L"UTF-8 split A2+1B");

    Check(Decode({0xff}, TextEncoding::Utf8) == L".", L"UTF-8 invalid FF");
    Check(Decode({0x80}, TextEncoding::Utf8) == L".", L"UTF-8 stray continuation");
    Check(Decode({0xc0, 0xaf}, TextEncoding::Utf8) == L"..", L"UTF-8 overlong");
    Check(Decode({0xed, 0xa0, 0x80}, TextEncoding::Utf8) == L"...", L"UTF-8 surrogate");
    Check(Decode({0xf4, 0x90, 0x80, 0x80}, TextEncoding::Utf8) == L"....",
          L"UTF-8 above Unicode range");
    Check(Decode({0x41, 0xe4, 0xbd, 0xa0, 0xff, 0x42}, TextEncoding::Utf8) == L"A你.B",
          L"UTF-8 local error recovery");

    CheckGbkSplit({{0xc4}, {0xe3}}, L"你", L"GBK split 1+1");
    CheckGbkSplit({{0x41, 0xc4}, {0xe3, 0x42}}, L"A你B", L"GBK even packet boundary");
    CheckGbkSplit({{0x41}, {0xc4}, {0xe3}, {0x42}}, L"A你B", L"GBK byte splits");
    CheckGbkSplit({{0xc4}}, L".", L"GBK incomplete flush");
    Check(Decode({0x81, 0x30, 0x41}, TextEncoding::Gbk) == L".0A", L"GBK invalid trail recovery");

    std::vector<std::uint8_t> bytes;
    std::wstring error;
    Check(!textcodec::Encode(L"\U0001f600", TextEncoding::Gbk, bytes, error) && !error.empty(),
          L"GBK rejects unrepresentable emoji");
    Check(!textcodec::Encode(L"中文", TextEncoding::Ascii, bytes, error) && !error.empty(),
          L"ASCII rejects non-ASCII");

    textcodec::RxTextDecoder rawDecoder(TextEncoding::Utf8);
    Check(rawDecoder.FeedBytes({0xe4, 0xbd}).empty() && rawDecoder.PendingSize() == 2,
          L"raw decoder retains UTF-8 suffix");
    Check(rawDecoder.FeedBytes({0xa0}) == std::vector<std::uint8_t>({0xe4, 0xbd, 0xa0}) &&
              rawDecoder.PendingSize() == 0,
          L"raw decoder emits combined UTF-8 bytes");

    if (failures == 0) std::wcout << L"All incremental text codec tests passed.\n";
    return failures == 0 ? 0 : 1;
}
