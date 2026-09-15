#include "LogWriter.h"

#include "Utilities.h"

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

std::vector<logdetail::StructuredRxChunk> logdetail::StructuredRxAssembler::Flush() {
    std::vector<StructuredRxChunk> output;
    auto bytes = decoder_.FlushBytes();
    if (!bytes.empty()) {
        comm::Record record;
        record.direction = comm::Direction::Rx;
        record.timestamp = pendingTimestamp_;
        record.rawBytes = std::move(bytes);
        output.push_back(StructuredRxChunk{std::move(record), encoding_});
    }
    pendingTimestamp_.clear();
    return output;
}

std::vector<logdetail::StructuredRxChunk> logdetail::StructuredRxAssembler::Feed(
    const comm::Record& record, textcodec::TextEncoding encoding) {
    std::vector<StructuredRxChunk> output;
    if (!initialized_ || encoding != encoding_) {
        if (initialized_) output = Flush();
        decoder_.Reset(encoding);
        encoding_ = encoding;
        initialized_ = true;
    }

    const bool hadPending = decoder_.PendingSize() != 0;
    const std::wstring previousTimestamp = pendingTimestamp_;
    auto ready = decoder_.FeedBytes(record.rawBytes);
    if (!ready.empty()) {
        comm::Record recordToWrite;
        recordToWrite.direction = comm::Direction::Rx;
        recordToWrite.timestamp = hadPending ? previousTimestamp : record.timestamp;
        recordToWrite.rawBytes = std::move(ready);
        output.push_back(StructuredRxChunk{std::move(recordToWrite), encoding_});
    }

    if (decoder_.PendingSize() != 0) {
        if (!hadPending || !output.empty()) pendingTimestamp_ = record.timestamp;
    } else {
        pendingTimestamp_.clear();
    }
    return output;
}

struct LogWriter::State final {
    struct RecordItem {
        comm::Record record;
        textcodec::TextEncoding encoding = textcodec::TextEncoding::Utf8;
    };

    State() : workerExited(CreateEventW(nullptr, TRUE, TRUE, nullptr)) {}
    ~State() { if (workerExited) CloseHandle(workerExited); }

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::string> queue;
    std::deque<RecordItem> recordQueue;
    std::ofstream stream;
    std::thread worker;
    HANDLE workerExited = nullptr;
    bool stopping = true;
    bool active = false;
    std::size_t queuedBytes = 0;
    std::size_t queuedRecordBytes = 0;
    std::uint64_t pendingDroppedBytes = 0;
    std::uint64_t pendingDroppedRecords = 0;
    std::uint64_t unreportedDroppedBytes = 0;
    std::uint64_t unreportedDroppedRecords = 0;
    logdetail::StructuredRxAssembler rxAssembler;
    static constexpr std::size_t kMaximumQueuedBytes = 8 * 1024 * 1024;

    template <typename RecordArg>
    void EnqueueRecord(RecordArg&& record, textcodec::TextEncoding encoding);
};

template <typename RecordArg>
void LogWriter::State::EnqueueRecord(RecordArg&& record, textcodec::TextEncoding encoding) {
    const std::size_t bytes = record.rawBytes.size() + record.timestamp.size() * sizeof(wchar_t) + 64;
    const std::size_t total = std::min(queuedBytes + queuedRecordBytes, kMaximumQueuedBytes);
    if (bytes > kMaximumQueuedBytes - total) {
        pendingDroppedBytes += record.rawBytes.size();
        ++pendingDroppedRecords;
        unreportedDroppedBytes += record.rawBytes.size();
        ++unreportedDroppedRecords;
        condition.notify_one();
        return;
    }
    queuedRecordBytes += bytes;
    recordQueue.push_back(RecordItem{std::forward<RecordArg>(record), encoding});
    condition.notify_one();
}

LogWriter::LogWriter() : state_(std::make_shared<State>()) {}

LogWriter::~LogWriter() {
    Stop();
}

bool LogWriter::Start(const std::wstring& path) {
    Stop();
    auto state = std::make_shared<State>();
    if (!state->workerExited) return false;
    state->stream.open(std::filesystem::path(path), std::ios::binary | std::ios::app);
    if (!state->stream) return false;
    ResetEvent(state->workerExited);
    state->stopping = false;
    state->active = true;
    try {
        state->worker = std::thread(&LogWriter::Worker, state);
    } catch (...) {
        state->stream.close();
        return false;
    }
    state_ = std::move(state);
    return true;
}

void LogWriter::Stop() {
    const auto state = state_;
    if (!state) return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stopping = true;
    }
    state->condition.notify_all();
    if (state->worker.joinable()) {
        if (state->workerExited && WaitForSingleObject(state->workerExited, 2500) == WAIT_OBJECT_0) {
            state->worker.join();
        } else {
            state->worker.detach();
        }
    }
    if (state_ == state) state_ = std::make_shared<State>();
}

void LogWriter::Write(std::string text) {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active || state->stopping) return;
    const std::size_t total = std::min(state->queuedBytes + state->queuedRecordBytes,
                                       State::kMaximumQueuedBytes);
    if (text.size() > State::kMaximumQueuedBytes - total) {
        state->pendingDroppedBytes += text.size();
        ++state->pendingDroppedRecords;
        state->unreportedDroppedBytes += text.size();
        ++state->unreportedDroppedRecords;
        state->condition.notify_one();
        return;
    }
    state->queuedBytes += text.size();
    state->queue.push_back(std::move(text));
    state->condition.notify_one();
}

void LogWriter::WriteRecord(const comm::Record& record, textcodec::TextEncoding encoding) {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active || state->stopping) return;
    state->EnqueueRecord(record, encoding);
}

void LogWriter::WriteRecord(comm::Record&& record, textcodec::TextEncoding encoding) {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active || state->stopping) return;
    state->EnqueueRecord(std::move(record), encoding);
}

LogWriter::OverflowStatus LogWriter::TakeOverflowStatus() {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    const OverflowStatus status{state->unreportedDroppedBytes, state->unreportedDroppedRecords};
    state->unreportedDroppedBytes = 0;
    state->unreportedDroppedRecords = 0;
    return status;
}

bool LogWriter::IsActive() const {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->active && !state->stopping;
}

void LogWriter::Worker(const std::shared_ptr<State>& state) {
    const auto writeFormatted = [&](const comm::Record& record, textcodec::TextEncoding encoding) {
        const auto text = comm::FormatRecord(record, comm::CopyFormat::Full, true, encoding);
        const auto utf8 = util::Utf8Bytes(text);
        state->stream.write(reinterpret_cast<const char*>(utf8.data()),
                            static_cast<std::streamsize>(utf8.size()));
    };
    const auto flushPendingRx = [&] {
        for (const auto& chunk : state->rxAssembler.Flush())
            writeFormatted(chunk.record, chunk.encoding);
    };
    for (;;) {
        std::deque<std::string> batch;
        std::deque<State::RecordItem> records;
        std::uint64_t droppedBytes = 0;
        std::uint64_t droppedRecords = 0;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->condition.wait(lock, [&] {
                return state->stopping || !state->queue.empty() || !state->recordQueue.empty() ||
                       state->pendingDroppedRecords != 0;
            });
            if (state->queue.empty() && state->recordQueue.empty() &&
                state->pendingDroppedRecords == 0 && state->stopping) break;
            batch.swap(state->queue);
            records.swap(state->recordQueue);
            state->queuedBytes = 0;
            state->queuedRecordBytes = 0;
            droppedBytes = state->pendingDroppedBytes;
            droppedRecords = state->pendingDroppedRecords;
            state->pendingDroppedBytes = 0;
            state->pendingDroppedRecords = 0;
        }
        if (droppedRecords != 0) {
            const std::string marker = "[WARNING] Log queue overflow, dropped " +
                std::to_string(droppedBytes) + " bytes / " +
                std::to_string(droppedRecords) + " records\r\n";
            state->stream.write(marker.data(), static_cast<std::streamsize>(marker.size()));
        }
        for (const auto& text : batch)
            state->stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        for (const auto& item : records) {
            if (item.record.direction != comm::Direction::Rx) {
                flushPendingRx();
                writeFormatted(item.record, item.encoding);
                continue;
            }
            for (const auto& chunk : state->rxAssembler.Feed(item.record, item.encoding))
                writeFormatted(chunk.record, chunk.encoding);
        }
        state->stream.flush();
    }
    flushPendingRx();
    state->stream.flush();
    state->stream.close();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->active = false;
    }
    if (state->workerExited) SetEvent(state->workerExited);
}
