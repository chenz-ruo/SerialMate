#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "TextCodec.h"

namespace comm {

constexpr std::size_t kBytesPerRow = 8;

enum class Direction {
    Rx,
    Tx,
    Notice,
    Error,
};

enum class RecordKind {
    Data,
    System,
};

enum class CopyFormat {
    Full,
    Hex,
    Text,
};

struct Record {
    std::uint64_t id = 0;
    std::wstring timestamp;
    Direction direction = Direction::Notice;
    RecordKind kind = RecordKind::Data;
    std::vector<std::uint8_t> rawBytes;
    std::wstring message;
    std::size_t firstRow = 0;
    std::array<std::size_t, 5> firstRows{};

    std::size_t RowCount(std::size_t bytesPerRow = kBytesPerRow) const;
};

struct VisualRow {
    const Record* record = nullptr;
    std::size_t offset = 0;
    std::size_t count = 0;
    bool first = false;
};

using TextRun = textcodec::TextRun;

// Decodes printable payload text without changing the raw-byte model. UTF-8
// is preferred; the active Windows code page is used as a GBK-compatible
// fallback. Invalid/control bytes are represented as '.'.
std::vector<TextRun> DisplayTextRuns(const std::vector<std::uint8_t>& bytes,
                                     textcodec::TextEncoding encoding = textcodec::TextEncoding::Utf8);
std::wstring DisplayText(const std::vector<std::uint8_t>& bytes,
                         textcodec::TextEncoding encoding = textcodec::TextEncoding::Utf8);
std::wstring DisplayTextRange(const std::vector<std::uint8_t>& bytes,
                              std::size_t offset, std::size_t count,
                              textcodec::TextEncoding encoding = textcodec::TextEncoding::Utf8);

// Formats one event without depending on the screen view. Full uses a stable
// 16-byte export layout; Hex/Text are one payload line from the raw bytes.
std::wstring FormatRecord(const Record& record,
                          CopyFormat format = CopyFormat::Full,
                          bool timestamps = true,
                          textcodec::TextEncoding encoding = textcodec::TextEncoding::Utf8,
                          std::size_t bytesPerRow = 16);

class RecordBuffer {
public:
    explicit RecordBuffer(std::size_t maxBytes = 4 * 1024 * 1024,
                          std::size_t maxRecords = 16384);

    const Record& Add(Direction direction, std::wstring timestamp,
                      std::vector<std::uint8_t> rawBytes);
    const Record& AddMessage(Direction direction, std::wstring timestamp,
                             std::wstring message);

    void Clear();
    std::size_t RowCount() const;
    std::size_t RowCount(std::size_t bytesPerRow) const;
    std::size_t ByteCount() const;
    std::size_t BaseRow() const;
    std::size_t BaseRow(std::size_t bytesPerRow) const;
    const std::deque<Record>& Records() const;
    // index values are local to the retained view (0..RowCount()-1).
    // Record::firstRow and BaseRow remain absolute for prefix accounting.
    std::optional<VisualRow> RowAt(std::size_t index) const;
    std::optional<VisualRow> RowAt(std::size_t index, std::size_t bytesPerRow) const;
    std::optional<std::size_t> RowOf(std::uint64_t id) const;
    std::optional<std::size_t> RowOf(std::uint64_t id, std::size_t bytesPerRow) const;
    std::wstring Copy(CopyFormat format,
                      std::optional<std::pair<std::uint64_t, std::uint64_t>> selection = std::nullopt,
                      bool timestamps = true,
                      textcodec::TextEncoding encoding = textcodec::TextEncoding::Utf8,
                      std::size_t bytesPerRow = 16) const;
private:
    const Record& AddRecord(Record record);
    void Trim();

    std::deque<Record> records_;
    std::size_t maxBytes_;
    std::size_t maxRecords_;
    std::size_t byteCount_ = 0;
    std::array<std::size_t, 5> rowCounts_{};
    std::array<std::size_t, 5> baseRows_{};
    std::uint64_t nextId_ = 1;
};

} // namespace comm
