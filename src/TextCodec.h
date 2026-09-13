#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace textcodec {

enum class TextEncoding { Utf8, Gbk, Ascii };

struct TextRun {
    std::size_t offset = 0;
    std::size_t byteLength = 0;
    std::wstring text;
};

const wchar_t* Name(TextEncoding encoding);
bool Encode(const std::wstring& text, TextEncoding encoding,
            std::vector<std::uint8_t>& bytes, std::wstring& error);
std::vector<TextRun> DecodeRuns(const std::vector<std::uint8_t>& bytes, TextEncoding encoding);
std::wstring Decode(const std::vector<std::uint8_t>& bytes, TextEncoding encoding);

class RxTextDecoder final {
public:
    explicit RxTextDecoder(TextEncoding encoding = TextEncoding::Utf8) : encoding_(encoding) {}
    void Reset(TextEncoding encoding);
    std::vector<std::uint8_t> FeedBytes(const std::vector<std::uint8_t>& bytes);
    std::vector<std::uint8_t> FlushBytes();
    std::wstring Feed(const std::vector<std::uint8_t>& bytes);
    std::wstring Flush();
    std::size_t PendingSize() const noexcept { return pending_.size(); }
private:
    TextEncoding encoding_;
    std::vector<std::uint8_t> pending_;
};

} // namespace textcodec
