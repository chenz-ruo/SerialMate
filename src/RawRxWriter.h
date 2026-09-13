#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class RawRxWriter final {
public:
    struct OverflowStatus {
        std::uint64_t bytes = 0;
        std::uint64_t chunks = 0;
    };

    RawRxWriter();
    ~RawRxWriter();
    RawRxWriter(const RawRxWriter&) = delete;
    RawRxWriter& operator=(const RawRxWriter&) = delete;

    bool Start(const std::wstring& path);
    void Stop();
    void Write(const std::vector<std::uint8_t>& bytes);
    bool IsActive() const;
    OverflowStatus TakeOverflowStatus();

private:
    struct State;
    static void Worker(const std::shared_ptr<State>& state);
    std::shared_ptr<State> state_;
};
