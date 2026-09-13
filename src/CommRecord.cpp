#include "CommRecord.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <stdexcept>

namespace comm {
namespace {

std::wstring HexBytes(const std::vector<std::uint8_t>& bytes) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring result;
    result.reserve(bytes.size() * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) result.push_back(L' ');
        result.push_back(digits[bytes[i] >> 4]);
        result.push_back(digits[bytes[i] & 0x0f]);
    }
    return result;
}

std::wstring AsciiBytes(const std::vector<std::uint8_t>& bytes, textcodec::TextEncoding encoding) {
    return textcodec::Decode(bytes, encoding);
}

void AppendHexRange(std::wstring& result, const std::vector<std::uint8_t>& bytes,
                    std::size_t offset, std::size_t count) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) result.push_back(L' ');
        const auto byte = bytes[offset + i];
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
}

std::wstring DirectionText(Direction direction) {
    switch (direction) {
    case Direction::Rx: return L"← RX";
    case Direction::Tx: return L"→ TX";
    case Direction::Error: return L"⚠ ERROR";
    case Direction::Notice: return L"• NOTICE";
    }
    return L"";
}

std::wstring Prefix(const Record& record, bool timestamps, bool first) {
    if (!first) {
        const std::wstring firstPrefix = timestamps
            ? L"[" + record.timestamp + L"]  " + DirectionText(record.direction) + L"  "
            : DirectionText(record.direction) + L"  ";
        return std::wstring(firstPrefix.size(), L' ');
    }
    if (timestamps) return L"[" + record.timestamp + L"]  " + DirectionText(record.direction) + L"  ";
    return DirectionText(record.direction) + L"  ";
}

std::wstring FullRecord(const Record& record, bool timestamps, textcodec::TextEncoding encoding, std::size_t bytesPerRow) {
    if (record.kind == RecordKind::System) {
        std::wstring line;
        if (timestamps && !record.timestamp.empty()) line = L"[" + record.timestamp + L"]  ";
        line += record.message;
        line += L"\r\n";
        return line;
    }

    bytesPerRow = std::max<std::size_t>(1, bytesPerRow);
    const std::size_t rows = (record.rawBytes.size() + bytesPerRow - 1) / bytesPerRow;
    std::wstring result;
    result.reserve(rows * (Prefix(record, timestamps, true).size() + kBytesPerRow * 4 + 8));
    for (std::size_t row = 0; row < rows; ++row) {
        const std::size_t offset = row * bytesPerRow;
        const std::size_t count = std::min(bytesPerRow, record.rawBytes.size() - offset);
        std::wstring hex;
        hex.reserve(kBytesPerRow * 3 - 1);
        AppendHexRange(hex, record.rawBytes, offset, count);
        hex.resize((bytesPerRow * 3) - 1, L' ');
        result += Prefix(record, timestamps, row == 0) + hex + L" │ ";
        result += DisplayTextRange(record.rawBytes, offset, count, encoding);
        result += L"\r\n";
    }
    return result;
}

bool ParseHexQuery(const std::wstring& text, std::vector<std::uint8_t>& bytes) {
    std::wstring compact;
    compact.reserve(text.size());
    for (const wchar_t ch : text) {
        if (iswspace(ch) || ch == L',' || ch == L'-' || ch == L':') continue;
        if (!iswxdigit(ch)) return false;
        compact.push_back(ch);
    }
    if (compact.empty() || (compact.size() % 2) != 0) return false;
    bytes.clear();
    bytes.reserve(compact.size() / 2);
    const auto nibble = [](wchar_t ch) -> unsigned {
        if (ch >= L'0' && ch <= L'9') return static_cast<unsigned>(ch - L'0');
        ch = static_cast<wchar_t>(towupper(ch));
        return static_cast<unsigned>(ch - L'A' + 10);
    };
    for (std::size_t i = 0; i < compact.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>((nibble(compact[i]) << 4) | nibble(compact[i + 1])));
    }
    return true;
}

bool ContainsBytes(const std::vector<std::uint8_t>& haystack,
                   const std::vector<std::uint8_t>& needle) {
    if (needle.empty()) return false;
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

std::size_t RowBucket(std::size_t bytesPerRow) {
    switch (bytesPerRow) {
    case 4: return 0;
    case 8: return 1;
    case 12: return 2;
    default: return 3;
    }
}

constexpr std::array<std::size_t, 4> kRowWidths{4, 8, 12, 16};

} // namespace

std::size_t Record::RowCount(std::size_t bytesPerRow) const {
    if (bytesPerRow == 0) return 0;
    return rawBytes.empty() ? 1 : (rawBytes.size() + bytesPerRow - 1) / bytesPerRow;
}

std::vector<TextRun> DisplayTextRuns(const std::vector<std::uint8_t>& bytes, textcodec::TextEncoding encoding) {
    return textcodec::DecodeRuns(bytes, encoding);
}

std::wstring DisplayText(const std::vector<std::uint8_t>& bytes, textcodec::TextEncoding encoding) {
    return textcodec::Decode(bytes, encoding);
}

std::wstring DisplayTextRange(const std::vector<std::uint8_t>& bytes,
                              std::size_t offset, std::size_t count,
                              textcodec::TextEncoding encoding) {
    if (offset >= bytes.size() || count == 0) return {};
    const std::size_t end = std::min(bytes.size(), offset + count);
    std::wstring result;
    for (const auto& run : textcodec::DecodeRuns(bytes, encoding)) {
        if (run.offset >= end) break;
        const std::size_t runEnd = run.offset + run.byteLength;
        if (runEnd <= offset) continue;
        if (run.offset < offset) {
            // A multibyte character belongs to the visual row containing its
            // first byte. Do not synthesize dots on continuation rows. Plain
            // one-byte runs remain sliceable so ASCII text stays continuous.
            if (run.byteLength == run.text.size()) {
                const std::size_t skip = offset - run.offset;
                const std::size_t take = std::min(end, runEnd) - offset;
                if (skip < run.text.size()) result.append(run.text, skip, take);
            }
        } else {
            // A decoded multibyte character belongs to the row containing its
            // first byte. Keep it visible even when the byte range ends in the
            // middle of that character; the continuation row remains empty.
            if (run.byteLength <= end - run.offset || run.byteLength != run.text.size()) {
                result += run.text;
            } else {
                result.append(run.text, 0, end - run.offset);
            }
        }
    }
    return result;
}

std::wstring FormatRecord(const Record& record, CopyFormat format, bool timestamps,
                          textcodec::TextEncoding encoding, std::size_t bytesPerRow) {
    switch (format) {
    case CopyFormat::Full:
        return FullRecord(record, timestamps, encoding, bytesPerRow);
    case CopyFormat::Hex:
        if (record.kind == RecordKind::System) return {};
        if (record.rawBytes.empty()) return {};
        return HexBytes(record.rawBytes) + L"\r\n";
    case CopyFormat::Text:
        if (record.kind == RecordKind::System) return {};
        if (record.rawBytes.empty()) return {};
        return AsciiBytes(record.rawBytes, encoding) + L"\r\n";
    }
    return {};
}

RecordBuffer::RecordBuffer(std::size_t maxBytes, std::size_t maxRecords)
    : maxBytes_(maxBytes), maxRecords_(maxRecords) {
    if (maxBytes_ == 0 || maxRecords_ == 0) {
        throw std::invalid_argument("RecordBuffer limits must be positive");
    }
}

const Record& RecordBuffer::Add(Direction direction, std::wstring timestamp,
                                std::vector<std::uint8_t> rawBytes) {
    if (rawBytes.size() > maxBytes_) {
        throw std::invalid_argument("record exceeds RecordBuffer byte limit");
    }
    Record record;
    record.id = nextId_++;
    record.timestamp = std::move(timestamp);
    record.direction = direction;
    record.kind = RecordKind::Data;
    record.rawBytes = std::move(rawBytes);
    return AddRecord(std::move(record));
}

const Record& RecordBuffer::AddMessage(Direction direction, std::wstring timestamp,
                                       std::wstring message) {
    if (message.size() > 4096) {
        throw std::invalid_argument("record message exceeds 4096 characters");
    }
    Record record;
    record.id = nextId_++;
    record.timestamp = std::move(timestamp);
    record.direction = direction;
    record.kind = RecordKind::System;
    record.message = std::move(message);
    return AddRecord(std::move(record));
}

const Record& RecordBuffer::AddRecord(Record record) {
    for (std::size_t index = 0; index < kRowWidths.size(); ++index) {
        record.firstRows[index] = baseRows_[index] + rowCounts_[index];
        rowCounts_[index] += record.RowCount(kRowWidths[index]);
    }
    record.firstRow = record.firstRows[RowBucket(kBytesPerRow)];
    byteCount_ += record.rawBytes.size();
    records_.push_back(std::move(record));
    Trim();
    return records_.back();
}

void RecordBuffer::Trim() {
    while (!records_.empty() && (byteCount_ > maxBytes_ || records_.size() > maxRecords_)) {
        const Record& oldest = records_.front();
        byteCount_ -= oldest.rawBytes.size();
        for (std::size_t index = 0; index < kRowWidths.size(); ++index) {
            const std::size_t rows = oldest.RowCount(kRowWidths[index]);
            rowCounts_[index] -= rows;
            baseRows_[index] += rows;
        }
        records_.pop_front();
    }
}

void RecordBuffer::Clear() {
    records_.clear();
    byteCount_ = 0;
    rowCounts_.fill(0);
    baseRows_.fill(0);
}

std::size_t RecordBuffer::RowCount() const { return rowCounts_[RowBucket(kBytesPerRow)]; }
std::size_t RecordBuffer::RowCount(std::size_t bytesPerRow) const {
    return rowCounts_[RowBucket(bytesPerRow)];
}
std::size_t RecordBuffer::ByteCount() const { return byteCount_; }
std::size_t RecordBuffer::BaseRow() const { return baseRows_[RowBucket(kBytesPerRow)]; }
std::size_t RecordBuffer::BaseRow(std::size_t bytesPerRow) const {
    return baseRows_[RowBucket(bytesPerRow)];
}
const std::deque<Record>& RecordBuffer::Records() const { return records_; }

std::optional<VisualRow> RecordBuffer::RowAt(std::size_t index) const {
    return RowAt(index, kBytesPerRow);
}

std::optional<VisualRow> RecordBuffer::RowAt(std::size_t index, std::size_t bytesPerRow) const {
    if (bytesPerRow == 0) return std::nullopt;
    const std::size_t bucket = RowBucket(bytesPerRow);
    if (index >= rowCounts_[bucket] || records_.empty()) return std::nullopt;
    const std::size_t absoluteIndex = baseRows_[bucket] + index;
    std::size_t low = 0;
    std::size_t high = records_.size();
    while (low + 1 < high) {
        const std::size_t mid = low + (high - low) / 2;
        if (records_[mid].firstRows[bucket] <= absoluteIndex) low = mid;
        else high = mid;
    }
    const Record& record = records_[low];
    const std::size_t local = absoluteIndex - record.firstRows[bucket];
    const std::size_t offset = record.rawBytes.empty() ? 0 : local * bytesPerRow;
    return VisualRow{&record, offset,
                     record.rawBytes.empty() ? 0 : std::min(bytesPerRow, record.rawBytes.size() - offset),
                     local == 0};
}

std::optional<std::size_t> RecordBuffer::RowOf(std::uint64_t id) const {
    return RowOf(id, kBytesPerRow);
}

std::optional<std::size_t> RecordBuffer::RowOf(std::uint64_t id, std::size_t bytesPerRow) const {
    const auto it = std::lower_bound(records_.begin(), records_.end(), id,
                                     [](const Record& record, std::uint64_t value) {
                                         return record.id < value;
                                     });
    if (it != records_.end() && it->id == id) {
        const std::size_t bucket = RowBucket(bytesPerRow);
        return it->firstRows[bucket] - baseRows_[bucket];
    }
    return std::nullopt;
}

std::wstring RecordBuffer::Copy(CopyFormat format,
                                std::optional<std::pair<std::uint64_t, std::uint64_t>> selection,
                                bool timestamps, textcodec::TextEncoding encoding, std::size_t bytesPerRow) const {
    std::wstring result;
    std::uint64_t first = 0;
    std::uint64_t last = std::numeric_limits<std::uint64_t>::max();
    if (selection) {
        first = std::min(selection->first, selection->second);
        last = std::max(selection->first, selection->second);
    }
    for (const auto& record : records_) {
        if (selection && (record.id < first || record.id > last)) continue;
        result += FormatRecord(record, format, timestamps, encoding, bytesPerRow);
    }
    return result;
}

const Record* RecordBuffer::FindNext(const std::wstring& query, std::uint64_t afterId,
                                     textcodec::TextEncoding encoding) const {
    if (records_.empty() || query.empty()) return nullptr;
    std::vector<std::uint8_t> hex;
    const bool validHex = ParseHexQuery(query, hex);
    const auto matches = [&](const Record& record) {
        if (validHex && ContainsBytes(record.rawBytes, hex)) return true;
        if (!record.rawBytes.empty() && AsciiBytes(record.rawBytes, encoding).find(query) != std::wstring::npos) return true;
        return record.message.find(query) != std::wstring::npos;
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& record : records_) {
            if (pass == 0 && record.id <= afterId) continue;
            if (pass == 1 && record.id > afterId) continue;
            if (matches(record)) return &record;
        }
    }
    return nullptr;
}

} // namespace comm
