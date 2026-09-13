#include "RxIngressQueue.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <iostream>
#include <thread>
#include <vector>

namespace {
int failures = 0;

void Check(bool condition, const char* name) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << name << '\n';
    }
}
}  // namespace

int main() {
    constexpr std::size_t kLimit = 4 * 1024 * 1024;
    RxIngressQueue queue(kLimit);
    std::atomic<int> notifications{0};
    queue.SetNotifier([&] {
        ++notifications;
        return true;
    });

    std::vector<std::uint8_t> block(1024, 0x5a);
    for (int i = 0; i < 100 * 1024; ++i) queue.Push(L"t", block);
    Check(queue.QueuedBytes() <= kLimit, "queue remains bounded");
    Check(queue.TakeReceivedBytes() == 100ULL * 1024ULL * 1024ULL,
          "received byte accounting");
    Check(queue.TakeDroppedBytes() > 0, "overflow is reported");
    Check(notifications.load() < 100 * 1024, "notifications are coalesced");

    std::deque<RxIngressRecord> drained;
    while (queue.QueuedBytes() != 0) queue.Drain(128 * 1024, drained);
    Check(!drained.empty(), "bounded queue drains records");

    RxIngressQueue raceQueue(kLimit);
    std::atomic<int> raceNotifications{0};
    std::atomic<bool> producerDone{false};
    raceQueue.SetNotifier([&] {
        ++raceNotifications;
        return true;
    });
    std::thread producer([&] {
        for (int i = 0; i < 20000; ++i) raceQueue.Push(L"t", block);
        producerDone.store(true);
    });
    std::thread consumer([&] {
        std::deque<RxIngressRecord> batch;
        while (!producerDone.load() || raceQueue.QueuedBytes() != 0) {
            raceQueue.Drain(64 * 1024, batch);
            if (raceQueue.QueuedBytes() == 0) std::this_thread::yield();
        }
    });
    producer.join();
    for (int i = 0; i < 100 && raceQueue.QueuedBytes() != 0; ++i) {
        std::deque<RxIngressRecord> batch;
        raceQueue.Drain(256 * 1024, batch);
    }
    consumer.join();
    Check(raceQueue.QueuedBytes() <= kLimit, "concurrent queue remains bounded");
    Check(raceNotifications.load() > 0, "concurrent notifications delivered");
    if (failures == 0) std::cout << "All RX ingress queue tests passed.\n";
    return failures == 0 ? 0 : 1;
}
