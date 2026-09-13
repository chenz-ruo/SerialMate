#include "TextCodec.h"

#include <windows.h>

#include <algorithm>

namespace textcodec {
namespace {

bool Printable(std::uint32_t codePoint) {
    return codePoint >= 0x20 && (codePoint < 0x7f || codePoint > 0x9f) &&
           codePoint != 0x2028 && codePoint != 0x2029;
}

std::wstring CodePoint(std::uint32_t value) {
    if (!Printable(value)) return L".";
    if (value <= 0xffff) return std::wstring(1, static_cast<wchar_t>(value));
    value -= 0x10000;
    return std::wstring{static_cast<wchar_t>(0xd800 + (value >> 10)),
                        static_cast<wchar_t>(0xdc00 + (value & 0x3ff))};
}

std::size_t Utf8Length(std::uint8_t first) {
    if (first < 0x80) return 1;
    if (first >= 0xc2 && first <= 0xdf) return 2;
    if (first >= 0xe0 && first <= 0xef) return 3;
    if (first >= 0xf0 && first <= 0xf4) return 4;
    return 0;
}

bool Continuation(std::uint8_t value) {
    return (value & 0xc0) == 0x80;
}

bool ValidSecond(std::uint8_t first, std::uint8_t second) {
    if (!Continuation(second)) return false;
    if (first == 0xe0) return second >= 0xa0;
    if (first == 0xed) return second <= 0x9f;
    if (first == 0xf0) return second >= 0x90;
    if (first == 0xf4) return second <= 0x8f;
    return true;
}

bool ValidUtf8(const std::vector<std::uint8_t>& bytes, std::size_t start,
               std::size_t length, std::uint32_t& value) {
    if (length == 0 || start + length > bytes.size()) return false;
    const std::uint8_t first = bytes[start];
    if (length == 1) {
        value = first;
        return first < 0x80;
    }
    if (!ValidSecond(first, bytes[start + 1])) return false;
    value = first & (length == 2 ? 0x1f : length == 3 ? 0x0f : 0x07);
    for (std::size_t index = 1; index < length; ++index) {
        if (!Continuation(bytes[start + index])) return false;
        value = (value << 6) | (bytes[start + index] & 0x3f);
    }
    return value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff);
}

bool Utf8PrefixCouldComplete(const std::vector<std::uint8_t>& bytes, std::size_t start,
                             std::size_t length) {
    const std::size_t available = bytes.size() - start;
    if (length < 2 || available >= length) return false;
    if (available >= 2 && !ValidSecond(bytes[start], bytes[start + 1])) return false;
    for (std::size_t index = 2; index < available; ++index) {
        if (!Continuation(bytes[start + index])) return false;
    }
    return true;
}

std::vector<TextRun> DecodeUtf8(const std::vector<std::uint8_t>& bytes) {
    std::vector<TextRun> runs;
    for (std::size_t pos = 0; pos < bytes.size();) {
        const std::size_t length = Utf8Length(bytes[pos]);
        std::uint32_t value = 0;
        if (length != 0 && ValidUtf8(bytes, pos, length, value)) {
            runs.push_back(TextRun{pos, length, CodePoint(value)});
            pos += length;
        } else {
            runs.push_back(TextRun{pos, 1, L"."});
            ++pos;
        }
    }
    return runs;
}

bool GbkLead(std::uint8_t value) {
    return value >= 0x81 && value <= 0xfe;
}

bool GbkTrail(std::uint8_t value) {
    return value >= 0x40 && value <= 0xfe && value != 0x7f;
}

std::vector<TextRun> DecodeCodePage(const std::vector<std::uint8_t>& bytes, UINT codePage,
                                    bool strictAscii) {
    std::vector<TextRun> runs;
    for (std::size_t pos = 0; pos < bytes.size();) {
        const std::size_t start = pos;
        const std::uint8_t first = bytes[pos];
        if (first < 0x80) {
            runs.push_back(TextRun{start, 1, CodePoint(first)});
            ++pos;
            continue;
        }
        if (strictAscii || !GbkLead(first) || pos + 1 >= bytes.size() ||
            !GbkTrail(bytes[pos + 1])) {
            runs.push_back(TextRun{start, 1, L"."});
            ++pos;
            continue;
        }
        wchar_t converted[2]{};
        const int count = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(bytes.data() + start), 2, converted, 2);
        if (count <= 0) {
            runs.push_back(TextRun{start, 1, L"."});
            ++pos;
            continue;
        }
        std::wstring text(converted, converted + count);
        bool printable = true;
        for (const wchar_t ch : text) {
            if (!Printable(static_cast<std::uint32_t>(ch))) printable = false;
        }
        runs.push_back(TextRun{start, 2, printable ? std::move(text) : L"."});
        pos += 2;
    }
    return runs;
}

std::size_t CompletePrefix(const std::vector<std::uint8_t>& bytes, TextEncoding encoding) {
    if (encoding == TextEncoding::Ascii) return bytes.size();
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const std::uint8_t first = bytes[pos];
        if (first < 0x80) {
            ++pos;
            continue;
        }
        if (encoding == TextEncoding::Gbk) {
            if (!GbkLead(first)) {
                ++pos;
                continue;
            }
            if (pos + 1 >= bytes.size()) return pos;
            pos += GbkTrail(bytes[pos + 1]) ? 2 : 1;
            continue;
        }
        const std::size_t length = Utf8Length(first);
        if (length == 0) {
            ++pos;
            continue;
        }
        if (pos + length > bytes.size()) {
            if (Utf8PrefixCouldComplete(bytes, pos, length)) return pos;
            ++pos;
            continue;
        }
        std::uint32_t value = 0;
        pos += ValidUtf8(bytes, pos, length, value) ? length : 1;
    }
    return pos;
}

} // namespace

const wchar_t* Name(TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Gbk: return L"GBK（简体中文）";
    case TextEncoding::Ascii: return L"ASCII（仅英文）";
    default: return L"UTF-8（推荐）";
    }
}

bool Encode(const std::wstring& text, TextEncoding encoding,
            std::vector<std::uint8_t>& bytes, std::wstring& error) {
    bytes.clear();
    error.clear();
    if (text.empty()) return true;
    if (encoding == TextEncoding::Ascii) {
        for (const wchar_t ch : text) {
            if (ch > 0x7f) {
                error = L"当前 ASCII 编码无法表示输入文本。\r\n请切换为 UTF-8 或 GBK。";
                return false;
            }
            bytes.push_back(static_cast<std::uint8_t>(ch));
        }
        return true;
    }

    const UINT codePage = encoding == TextEncoding::Utf8 ? CP_UTF8 : 936;
    const DWORD flags = encoding == TextEncoding::Utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL usedDefault = FALSE;
    BOOL* usedDefaultPointer = encoding == TextEncoding::Gbk ? &usedDefault : nullptr;
    const int count = WideCharToMultiByte(codePage, flags, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, usedDefaultPointer);
    if (count <= 0 || usedDefault) {
        error = encoding == TextEncoding::Gbk
            ? L"当前 GBK 编码无法表示输入文本。\r\n请切换为 UTF-8。"
            : L"当前 UTF-8 编码无法表示输入文本。";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(count));
    usedDefault = FALSE;
    if (WideCharToMultiByte(codePage, flags, text.data(), static_cast<int>(text.size()),
                            reinterpret_cast<char*>(bytes.data()), count, nullptr,
                            usedDefaultPointer) <= 0 || usedDefault) {
        bytes.clear();
        error = encoding == TextEncoding::Gbk
            ? L"当前 GBK 编码无法表示输入文本。\r\n请切换为 UTF-8。"
            : L"当前 UTF-8 编码无法表示输入文本。";
        return false;
    }
    return true;
}

std::vector<TextRun> DecodeRuns(const std::vector<std::uint8_t>& bytes, TextEncoding encoding) {
    if (encoding == TextEncoding::Utf8) return DecodeUtf8(bytes);
    if (encoding == TextEncoding::Ascii) return DecodeCodePage(bytes, 20127, true);
    return DecodeCodePage(bytes, 936, false);
}

std::wstring Decode(const std::vector<std::uint8_t>& bytes, TextEncoding encoding) {
    std::wstring result;
    for (const auto& run : DecodeRuns(bytes, encoding)) result += run.text;
    return result;
}

void RxTextDecoder::Reset(TextEncoding encoding) {
    encoding_ = encoding;
    pending_.clear();
}

std::vector<std::uint8_t> RxTextDecoder::FeedBytes(const std::vector<std::uint8_t>& bytes) {
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    const std::size_t complete = CompletePrefix(pending_, encoding_);
    std::vector<std::uint8_t> ready(pending_.begin(), pending_.begin() + complete);
    pending_.erase(pending_.begin(), pending_.begin() + complete);
    return ready;
}

std::vector<std::uint8_t> RxTextDecoder::FlushBytes() {
    auto ready = std::move(pending_);
    pending_.clear();
    return ready;
}

std::wstring RxTextDecoder::Feed(const std::vector<std::uint8_t>& bytes) {
    return Decode(FeedBytes(bytes), encoding_);
}

std::wstring RxTextDecoder::Flush() {
    return Decode(FlushBytes(), encoding_);
}

} // namespace textcodec
