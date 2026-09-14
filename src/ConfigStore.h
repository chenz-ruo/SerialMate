#pragma once

#include <array>
#include <bitset>
#include <filesystem>
#include <string>

namespace config {

inline constexpr int kConfigVersion = 1;
inline constexpr std::size_t kSlotCount = 16;
inline constexpr std::size_t kMaximumCustomDataLength = 4096;

struct ConfigData {
    std::array<std::wstring, kSlotCount> customData{};
};

class ConfigStore final {
public:
    static ConfigStore Open(const std::filesystem::path& primary,
                            const std::filesystem::path& fallback);
    static ConfigStore OpenDefault();

    const ConfigData& InitialData() const { return initialData_; }
    const std::filesystem::path& Path() const { return path_; }
    bool UsingFallback() const { return usingFallback_; }
    bool SaveMerged(const ConfigData& data, const std::bitset<kSlotCount>& dirty,
                    std::wstring& error);

private:
    ConfigStore(std::filesystem::path path, ConfigData initialData, bool usingFallback)
        : path_(std::move(path)), initialData_(std::move(initialData)),
          usingFallback_(usingFallback) {}

    std::filesystem::path path_;
    ConfigData initialData_{};
    bool usingFallback_ = false;
};

} // namespace config
