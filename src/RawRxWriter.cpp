#include "RawRxWriter.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

struct RawRxWriter::State final {
    State() : workerExited(CreateEventW(nullptr, TRUE, TRUE, nullptr)) {}
    ~State() { if (workerExited) CloseHandle(workerExited); }

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::vector<std::uint8_t>> queue;
    std::ofstream stream;
    std::thread worker;
    HANDLE workerExited = nullptr;
    bool stopping = true;
    bool active = false;
    std::size_t queuedBytes = 0;
    std::uint64_t droppedBytes = 0;
    std::uint64_t droppedChunks = 0;
    static constexpr std::size_t kMaximumQueuedBytes = 8 * 1024 * 1024;
};

void RawRxWriter::Worker(const std::shared_ptr<State>& state) {
    for (;;) {
        std::deque<std::vector<std::uint8_t>> batch;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->condition.wait(lock, [&] { return state->stopping || !state->queue.empty(); });
            if (state->queue.empty() && state->stopping) break;
            batch.swap(state->queue);
            state->queuedBytes = 0;
        }
        for (const auto& bytes : batch) {
            state->stream.write(reinterpret_cast<const char*>(bytes.data()),
                                static_cast<std::streamsize>(bytes.size()));
        }
        state->stream.flush();
    }
    state->stream.close();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->active = false;
    }
    if (state->workerExited) SetEvent(state->workerExited);
}

RawRxWriter::RawRxWriter() : state_(std::make_shared<State>()) {}

RawRxWriter::~RawRxWriter() {
    Stop();
}

bool RawRxWriter::Start(const std::wstring& path) {
    Stop();
    auto state = std::make_shared<State>();
    if (!state->workerExited) return false;
    state->stream.open(std::filesystem::path(path), std::ios::binary | std::ios::app);
    if (!state->stream) return false;
    ResetEvent(state->workerExited);
    state->stopping = false;
    state->active = true;
    try {
        state->worker = std::thread(&RawRxWriter::Worker, state);
    } catch (...) {
        state->stream.close();
        return false;
    }
    state_ = std::move(state);
    return true;
}

void RawRxWriter::Stop() {
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

void RawRxWriter::Write(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return;
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active || state->stopping) return;
    if (bytes.size() > State::kMaximumQueuedBytes - std::min(state->queuedBytes, State::kMaximumQueuedBytes)) {
        state->droppedBytes += bytes.size();
        ++state->droppedChunks;
        return;
    }
    state->queuedBytes += bytes.size();
    state->queue.push_back(bytes);
    state->condition.notify_one();
}

bool RawRxWriter::IsActive() const {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->active && !state->stopping;
}

RawRxWriter::OverflowStatus RawRxWriter::TakeOverflowStatus() {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    const OverflowStatus status{state->droppedBytes, state->droppedChunks};
    state->droppedBytes = 0;
    state->droppedChunks = 0;
    return status;
}
