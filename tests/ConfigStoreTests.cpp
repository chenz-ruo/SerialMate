#include "ConfigStore.h"

#include <windows.h>

#include <bitset>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

class TempDirectory final {
public:
    TempDirectory() {
        wchar_t root[MAX_PATH]{};
        GetTempPathW(static_cast<DWORD>(std::size(root)), root);
        path_ = std::filesystem::path(root) /
            (L"SerialMate-ConfigStoreTests-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

void WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Check(static_cast<bool>(output), "cannot write test fixture");
}

std::vector<char> ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(input), {});
}

void TestLoadingAndPaths() {
    TempDirectory temporary;
    const auto primary = temporary.Path() / L"portable" / L"SerialMate.ini";
    const auto fallback = temporary.Path() / L"local" / L"SerialMate" / L"SerialMate.ini";
    std::filesystem::create_directories(primary.parent_path());

    auto empty = config::ConfigStore::Open(primary, fallback);
    Check(empty.Path() == primary && !empty.UsingFallback(), "new writable install must prefer executable directory");
    for (const auto& value : empty.InitialData().customData)
        Check(value.empty(), "missing configuration must use empty defaults");

    WriteBytes(primary,
        "ConfigVersion=99\r\n"
        "CustomData01=" + std::string(u8"中文 = 01 03") + "\r\n"
        "CustomData02=AT+VER?\r\n"
        "UnknownField=ignored\r\n"
        "CustomData16=last\r\n");
    auto loaded = config::ConfigStore::Open(primary, fallback);
    Check(loaded.InitialData().customData[0] == L"中文 = 01 03", "UTF-8 or equals sign was not preserved");
    Check(loaded.InitialData().customData[1] == L"AT+VER?", "ASCII custom data was not loaded");
    Check(loaded.InitialData().customData[15] == L"last", "sixteenth slot was not loaded");

    std::string damaged = "ConfigVersion=1\r\nCustomData01=good\r\nCustomData02=";
    damaged.push_back(static_cast<char>(0xC3));
    damaged += "\r\nCustomData03=" + std::string(4097, 'X') + "\r\nCustomData04=still-good\r\n";
    WriteBytes(primary, damaged);
    auto partial = config::ConfigStore::Open(primary, fallback);
    Check(partial.InitialData().customData[0] == L"good", "valid field before damage was lost");
    Check(partial.InitialData().customData[1].empty(), "invalid UTF-8 field did not default");
    Check(partial.InitialData().customData[2].empty(), "overlong field did not default");
    Check(partial.InitialData().customData[3] == L"still-good", "valid field after damage was lost");

    WriteBytes(primary, "ConfigVersion=1\r\nCustomData01=portable\r\nCustomData02=portable-two\r\n");
    WriteBytes(fallback, "ConfigVersion=1\r\nCustomData01=local\r\nCustomData16=local-last\r\n");
    SetFileAttributesW(primary.c_str(), FILE_ATTRIBUTE_READONLY);
    auto redirected = config::ConfigStore::Open(primary, fallback);
    Check(redirected.UsingFallback() && redirected.Path() == fallback,
          "unwritable executable configuration did not select fallback");
    Check(redirected.InitialData().customData[0] == L"local" &&
          redirected.InitialData().customData[1] == L"portable-two" &&
          redirected.InitialData().customData[15] == L"local-last",
          "fallback fields did not overlay the portable base");
    SetFileAttributesW(primary.c_str(), FILE_ATTRIBUTE_NORMAL);

    HANDLE readOnlyAccess = CreateFileW(primary.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(readOnlyAccess != INVALID_HANDLE_VALUE, "cannot prepare write-denied primary fixture");
    auto accessRedirected = config::ConfigStore::Open(primary, fallback);
    Check(accessRedirected.UsingFallback() && accessRedirected.Path() == fallback,
          "primary that cannot be opened for writing did not select fallback");
    if (readOnlyAccess != INVALID_HANDLE_VALUE) CloseHandle(readOnlyAccess);
}

void TestSavingAndMerge() {
    TempDirectory temporary;
    const auto primary = temporary.Path() / L"portable" / L"SerialMate.ini";
    const auto fallback = temporary.Path() / L"local" / L"SerialMate" / L"SerialMate.ini";
    std::filesystem::create_directories(primary.parent_path());
    auto first = config::ConfigStore::Open(primary, fallback);
    auto second = config::ConfigStore::Open(primary, fallback);
    auto firstData = first.InitialData();
    auto secondData = second.InitialData();
    firstData.customData[0] = L"甲";
    secondData.customData[15] = L"01 03 00 00";
    std::bitset<config::kSlotCount> firstDirty;
    std::bitset<config::kSlotCount> secondDirty;
    firstDirty.set(0);
    secondDirty.set(15);
    std::wstring error;
    Check(first.SaveMerged(firstData, firstDirty, error), "first merged save failed");
    Check(second.SaveMerged(secondData, secondDirty, error), "second merged save failed");
    const auto merged = config::ConfigStore::Open(primary, fallback).InitialData();
    Check(merged.customData[0] == L"甲" && merged.customData[15] == L"01 03 00 00",
          "independent instance changes overwrote each other");

    const auto bytes = ReadBytes(primary);
    const std::string text(bytes.begin(), bytes.end());
    Check(bytes.size() < 3 || static_cast<unsigned char>(bytes[0]) != 0xEF ||
          static_cast<unsigned char>(bytes[1]) != 0xBB || static_cast<unsigned char>(bytes[2]) != 0xBF,
          "configuration contains a UTF-8 BOM");
    Check(text.find("ConfigVersion=1\r\n") == 0 && text.find("CustomData16=") != std::string::npos,
          "configuration format or CRLF is incorrect");
    const DWORD attributes = GetFileAttributesW(primary.c_str());
    Check(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0,
          "configuration is not hidden");
    Check(!std::filesystem::exists(primary.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId())),
          "successful save left a temporary file");

    auto maximum = merged;
    maximum.customData[7].assign(config::kMaximumCustomDataLength, L'测');
    std::bitset<config::kSlotCount> maximumDirty;
    maximumDirty.set(7);
    Check(first.SaveMerged(maximum, maximumDirty, error), "maximum-length value did not save");
    Check(config::ConfigStore::Open(primary, fallback).InitialData().customData[7].size() ==
          config::kMaximumCustomDataLength, "maximum-length value did not round trip");

    Check(SetFileAttributesW(primary.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE) != FALSE,
          "cannot prepare archive attribute fixture");
    maximum.customData[8] = L"保留属性";
    std::bitset<config::kSlotCount> archiveDirty;
    archiveDirty.set(8);
    Check(first.SaveMerged(maximum, archiveDirty, error), "save with archive attribute failed");
    const DWORD preservedAttributes = GetFileAttributesW(primary.c_str());
    Check(preservedAttributes != INVALID_FILE_ATTRIBUTES &&
          (preservedAttributes & FILE_ATTRIBUTE_HIDDEN) != 0 &&
          (preservedAttributes & FILE_ATTRIBUTE_ARCHIVE) != 0,
          "save did not preserve archive and hidden attributes");

    auto earlier = config::ConfigStore::Open(primary, fallback);
    auto later = config::ConfigStore::Open(primary, fallback);
    auto earlierData = earlier.InitialData();
    auto laterData = later.InitialData();
    earlierData.customData[4] = L"较早写入";
    laterData.customData[4] = L"最后写入";
    std::bitset<config::kSlotCount> sameSlotDirty;
    sameSlotDirty.set(4);
    Check(earlier.SaveMerged(earlierData, sameSlotDirty, error), "earlier same-slot save failed");
    Check(later.SaveMerged(laterData, sameSlotDirty, error), "later same-slot save failed");
    Check(config::ConfigStore::Open(primary, fallback).InitialData().customData[4] == L"最后写入",
          "last successful same-slot writer did not win");

    const auto originalBytes = ReadBytes(primary);
    HANDLE blockingHandle = CreateFileW(primary.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(blockingHandle != INVALID_HANDLE_VALUE, "cannot lock target against atomic replacement");
    auto blockedData = later.InitialData();
    blockedData.customData[2] = L"不得覆盖";
    std::bitset<config::kSlotCount> blockedDirty;
    blockedDirty.set(2);
    Check(!later.SaveMerged(blockedData, blockedDirty, error),
          "save unexpectedly succeeded while target replacement was blocked");
    if (blockingHandle != INVALID_HANDLE_VALUE) CloseHandle(blockingHandle);
    Check(ReadBytes(primary) == originalBytes, "failed replacement changed the original file");
    Check(!std::filesystem::exists(primary.wstring() + L".tmp." +
                                   std::to_wstring(GetCurrentProcessId())),
          "failed replacement left a temporary file");
}
} // namespace

int wmain() {
    TestLoadingAndPaths();
    TestSavingAndMerge();
    if (failures == 0) std::cout << "All ConfigStore tests passed.\n";
    return failures == 0 ? 0 : 1;
}
