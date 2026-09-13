#include "../src/UpdateChecker.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* name) {
    if (!condition) {
        ++failures;
        std::wcerr << L"FAIL: " << name << L'\n';
    }
}

std::vector<std::uint8_t> Bytes(const std::string& value) {
    return {value.begin(), value.end()};
}

std::string Manifest(const std::string& version, const std::string& url,
                     const std::string& sha256, const std::string& size) {
    return "version=" + version + "\r\nurl=" + url + "\r\nsha256=" + sha256 +
           "\r\nsize=" + size + "\r\n";
}

}  // namespace

int wmain() {
    const std::vector<std::uint8_t> payload = Bytes("verified update payload");
    const std::wstring hash = Sha256Hex(payload);
    Check(hash.size() == 64, L"SHA-256 输出长度");

    UpdateManifest parsed;
    std::wstring error;
    std::string hashText;
    hashText.reserve(hash.size());
    for (const wchar_t ch : hash) hashText.push_back(static_cast<char>(ch));
    Check(ParseUpdateManifest(Bytes(Manifest("1.1.2", "https://example.com/update.exe", hashText,
                                             std::to_string(payload.size()))), parsed, error),
          L"有效更新清单");
    Check(parsed.version == L"1.1.2" && parsed.size == payload.size(), L"清单字段解析");
    Check(VerifyUpdatePayload(payload, parsed, error), L"更新文件校验成功");

    auto altered = payload;
    altered.back() ^= 1;
    Check(!VerifyUpdatePayload(altered, parsed, error), L"SHA-256 不匹配必须拒绝");
    parsed.size += 1;
    Check(!VerifyUpdatePayload(payload, parsed, error), L"文件大小不匹配必须拒绝");

    UpdateManifest unused;
    Check(!ParseUpdateManifest(Bytes("<html>error</html>"), unused, error), L"HTML 响应拒绝");
    Check(!ParseUpdateManifest({}, unused, error), L"空清单拒绝");
    Check(!ParseUpdateManifest(Bytes(Manifest("1.1.2", "http://example.com/a.exe", hashText, "1")),
                               unused, error), L"HTTP 下载地址拒绝");
    Check(!ParseUpdateManifest(Bytes(Manifest("1.1.2", "https://example.com/a.exe", "abcd", "1")),
                               unused, error), L"无效 SHA-256 拒绝");
    Check(!ParseUpdateManifest(Bytes(Manifest("1.1.2", "https://example.com/a.exe", hashText, "0")),
                               unused, error), L"零大小拒绝");
    Check(!ParseUpdateManifest(std::vector<std::uint8_t>(4097, 'x'), unused, error), L"超大清单拒绝");

    UpdateManifest gitee{L"1.1.4", L"https://gitee.com/a.exe", hash, payload.size()};
    UpdateManifest github{L"1.1.4", L"https://github.com/a.exe", hash, payload.size()};
    UpdateManifest selected;
    Check(SelectUpdateManifest(&gitee, &github, L"1.1.3", selected) &&
              selected.url == gitee.url && selected.fallbackUrl == github.url,
          L"相同版本允许镜像 fallback，并优先 Gitee");

    github.version = L"1.1.3";
    Check(SelectUpdateManifest(&gitee, &github, L"1.1.2", selected) &&
              selected.url == gitee.url && selected.fallbackUrl.empty(),
          L"Gitee 新版本不能 fallback 到较旧 GitHub");

    gitee.version = L"1.1.3";
    github.version = L"1.1.4";
    Check(SelectUpdateManifest(&gitee, &github, L"1.1.2", selected) &&
              selected.url == github.url && selected.fallbackUrl.empty(),
          L"GitHub 新版本不能 fallback 到较旧 Gitee");

    Check(!SelectUpdateManifest(&gitee, &github, L"1.1.4", selected),
          L"相同当前版本不提示更新");

    UpdateChecker checker;
    std::atomic_int callbacks{0};
    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        checker.Start([&](UpdateManifest) { callbacks.fetch_add(1); });
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        checker.Stop();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        Check(elapsed <= std::chrono::seconds(3), L"联网检查取消不超过 3 秒");
    }
    if (failures != 0) return 1;
    std::wcout << L"PASS: update manifest, SHA-256, size checks and cancellation; callbacks="
               << callbacks.load() << L'\n';
    return 0;
}
