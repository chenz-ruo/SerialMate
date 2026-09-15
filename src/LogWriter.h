#pragma once

#include "CommRecord.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace logdetail {

struct StructuredRxChunk {
    comm::Record record;
    textcodec::TextEncoding encoding = textcodec::TextEncoding::Utf8;
};

class StructuredRxAssembler final {
public:
    std::vector<StructuredRxChunk> Feed(const comm::Record& record,
                                        textcodec::TextEncoding encoding);
    std::vector<StructuredRxChunk> Flush();
    std::size_t PendingSize() const noexcept { return decoder_.PendingSize(); }

private:
    textcodec::RxTextDecoder decoder_;
    textcodec::TextEncoding encoding_ = textcodec::TextEncoding::Utf8;
    std::wstring pendingTimestamp_;
    bool initialized_ = false;
};

}  // namespace logdetail

class LogWriter final {
public:
    struct OverflowStatus {
        std::uint64_t bytes = 0;
        std::uint64_t records = 0;
    };

    LogWriter();
    ~LogWriter();
    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    bool Start(const std::wstring& path);
    void Stop();
    void Write(std::string text);
    void WriteRecord(const comm::Record& record,
                     textcodec::TextEncoding encoding = textcodec::TextEncoding::Utf8);
    void WriteRecord(comm::Record&& record,
                     textcodec::TextEncoding encoding = textcodec::TextEncoding::Utf8);
    bool IsActive() const;
    OverflowStatus TakeOverflowStatus();

private:
    struct State;
    static void Worker(const std::shared_ptr<State>& state);
    std::shared_ptr<State> state_;
};
