#include "RxIngressQueue.h"

#include <algorithm>
#include <stdexcept>

RxIngressQueue::RxIngressQueue(std::size_t maximumBytes)
    : maximumBytes_(maximumBytes) {
    if (maximumBytes_ == 0) throw std::invalid_argument("RxIngressQueue limit must be positive");
}

void RxIngressQueue::SetNotifier(Notify notify) {
    std::lock_guard<std::mutex> lock(mutex_);
    notify_ = std::move(notify);
}

void RxIngressQueue::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
    notify_ = {};
    notificationPending_ = false;
}

void RxIngressQueue::Push(std::wstring timestamp, std::vector<std::uint8_t> bytes) {
    if (bytes.empty()) return;
    Notify notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) return;
        receivedBytes_ += bytes.size();
        while (!queue_.empty() && queuedBytes_ + bytes.size() > maximumBytes_) {
            droppedBytes_ += queue_.front().rawBytes.size();
            queuedBytes_ -= queue_.front().rawBytes.size();
            queue_.pop_front();
        }
        if (bytes.size() > maximumBytes_) {
            droppedBytes_ += bytes.size();
            return;
        }
        queuedBytes_ += bytes.size();
        queue_.push_back(RxIngressRecord{std::move(timestamp), std::move(bytes)});
        if (!notificationPending_) {
            notificationPending_ = true;
            notify = notify_;
        }
    }
    if (notify) PostPendingNotification();
}

std::size_t RxIngressQueue::Drain(std::size_t maximumBytes, std::deque<RxIngressRecord>& output) {
    if (maximumBytes == 0) return 0;
    bool notifyAgain = false;
    std::size_t drained = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty() && (drained == 0 || drained + queue_.front().rawBytes.size() <= maximumBytes)) {
            drained += queue_.front().rawBytes.size();
            queuedBytes_ -= queue_.front().rawBytes.size();
            output.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        if (queue_.empty()) {
            notificationPending_ = false;
        } else {
            notifyAgain = true;
        }
    }
    if (notifyAgain) PostPendingNotification();
    return drained;
}

std::uint64_t RxIngressQueue::TakeDroppedBytes() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto result = droppedBytes_;
    droppedBytes_ = 0;
    return result;
}

std::uint64_t RxIngressQueue::TakeReceivedBytes() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto result = receivedBytes_;
    receivedBytes_ = 0;
    return result;
}

std::size_t RxIngressQueue::QueuedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queuedBytes_;
}

bool RxIngressQueue::NotificationPending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return notificationPending_;
}

void RxIngressQueue::PostPendingNotification() {
    for (int attempt = 0; attempt < 4; ++attempt) {
        Notify notify;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_ || queue_.empty() || !notificationPending_) return;
            notify = notify_;
            if (!notify) return;
        }
        if (notify()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) {
            notificationPending_ = false;
            return;
        }
        // Clear and retry under the same mutex that guards queue emptiness;
        // a concurrent producer will either post or observe the new pending
        // state, so no push can be stranded without a notification.
        notificationPending_ = false;
        if (queue_.empty()) return;
        notificationPending_ = true;
    }
}
