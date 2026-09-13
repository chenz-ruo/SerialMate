#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct RxIngressRecord final {
    std::wstring timestamp;
    std::vector<std::uint8_t> rawBytes;
};

// Bounded producer-side receive queue. It owns the single pending UI
// notification invariant; producers never allocate a message payload.
class RxIngressQueue final {
public:
    using Notify = std::function<bool()>;

    explicit RxIngressQueue(std::size_t maximumBytes = 4 * 1024 * 1024);

    void SetNotifier(Notify notify);
    void Stop();
    void Push(std::wstring timestamp, std::vector<std::uint8_t> bytes);
    std::size_t Drain(std::size_t maximumBytes, std::deque<RxIngressRecord>& output);
    std::uint64_t TakeDroppedBytes();
    std::uint64_t TakeReceivedBytes();
    std::size_t QueuedBytes() const;
    std::size_t MaximumBytes() const { return maximumBytes_; }
    bool NotificationPending() const;

private:
    void PostPendingNotification();

    mutable std::mutex mutex_;
    std::deque<RxIngressRecord> queue_;
    Notify notify_;
    const std::size_t maximumBytes_;
    std::size_t queuedBytes_ = 0;
    std::uint64_t droppedBytes_ = 0;
    std::uint64_t receivedBytes_ = 0;
    bool accepting_ = true;
    bool notificationPending_ = false;
};
