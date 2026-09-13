#pragma once

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct UpdateManifest {
    std::wstring version;
    std::wstring url;
    std::wstring sha256;
    std::uint64_t size = 0;
    // Optional independently validated fallback asset from the other source.
    std::wstring fallbackUrl;
    std::wstring fallbackSha256;
    std::uint64_t fallbackSize = 0;
};

struct UpdateDownloadResult {
    bool success = false;
    UpdateManifest manifest;
    std::wstring path;
    std::wstring error;
};

bool ParseUpdateManifest(const std::vector<std::uint8_t>& bytes,
                         UpdateManifest& manifest, std::wstring& error);
std::wstring Sha256Hex(const std::vector<std::uint8_t>& bytes);
bool VerifyUpdatePayload(const std::vector<std::uint8_t>& bytes,
                         const UpdateManifest& manifest, std::wstring& error);
bool SelectUpdateManifest(const UpdateManifest* gitee, const UpdateManifest* github,
                          const std::wstring& currentVersion, UpdateManifest& selected);

class UpdateChecker final {
public:
    using CheckCallback = std::function<void(UpdateManifest)>;
    using DownloadCallback = std::function<void(UpdateDownloadResult)>;

    UpdateChecker() = default;
    ~UpdateChecker();
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    void Start(CheckCallback callback);
    void Download(UpdateManifest manifest, std::wstring destination,
                  DownloadCallback callback);
    void Stop();

private:
    enum class Job { Check, Download };
    struct State;
    static bool FetchUrl(const std::shared_ptr<State>& state, const std::wstring& url, std::size_t maximumBytes,
                  std::vector<std::uint8_t>& content, std::wstring& error);
    static void Run(const std::shared_ptr<State>& state);

    std::shared_ptr<State> state_;
    std::thread thread_;
};
