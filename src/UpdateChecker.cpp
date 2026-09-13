#include "UpdateChecker.h"

#include "Utilities.h"
#include "Version.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <system_error>

namespace {

constexpr std::size_t kMaximumManifestBytes = 4096;
constexpr std::uint64_t kMaximumExecutableBytes = 64ull * 1024ull * 1024ull;

std::wstring Trim(std::wstring text) {
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    return text;
}

bool IsVersion(const std::wstring& value) {
    return util::IsNewerVersion(value, L"0.0.0") || value == L"0.0" ||
           value == L"0.0.0" || value == L"0.0.0.0";
}

bool IsHttpsUrl(const std::wstring& value) {
    return value.size() > 8 && _wcsnicmp(value.c_str(), L"https://", 8) == 0;
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

std::wstring Hex(const std::uint8_t* bytes, std::size_t count) {
    static constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring result;
    result.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(digits[bytes[i] >> 4]);
        result.push_back(digits[bytes[i] & 0x0f]);
    }
    return result;
}

}  // namespace

struct UpdateChecker::State final {
    Job job = Job::Check;
    CheckCallback checkCallback;
    DownloadCallback downloadCallback;
    UpdateManifest manifest;
    std::wstring destination;
    std::atomic_bool stopping{false};
    std::atomic<HINTERNET> session{nullptr};
    std::atomic<HINTERNET> request{nullptr};
    HANDLE workerExited = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    std::mutex mutex;
    ~State() { if (workerExited) CloseHandle(workerExited); }
};

bool ParseUpdateManifest(const std::vector<std::uint8_t>& bytes,
                         UpdateManifest& manifest, std::wstring& error) {
    manifest = {};
    if (bytes.empty() || bytes.size() > kMaximumManifestBytes) {
        error = L"更新清单为空或超过大小限制。";
        return false;
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           reinterpret_cast<const char*>(bytes.data()),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) {
        error = L"更新清单不是有效的 UTF-8。";
        return false;
    }
    std::wstring text(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        reinterpret_cast<const char*>(bytes.data()),
                        static_cast<int>(bytes.size()), text.data(), count);
    if (!text.empty() && text.front() == 0xfeff) text.erase(text.begin());

    std::map<std::wstring, std::wstring> values;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(L'\n', start);
        std::wstring line = Trim(text.substr(start, end == std::wstring::npos ? end : end - start));
        if (!line.empty() && line.front() != L'#') {
            const std::size_t equals = line.find(L'=');
            if (equals == std::wstring::npos) {
                error = L"更新清单包含无效行。";
                return false;
            }
            const std::wstring key = Lower(Trim(line.substr(0, equals)));
            const std::wstring value = Trim(line.substr(equals + 1));
            if (key.empty() || value.empty() || values.find(key) != values.end()) {
                error = L"更新清单包含空值或重复字段。";
                return false;
            }
            values.emplace(key, value);
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (values.size() != 4 || values.count(L"version") == 0 || values.count(L"url") == 0 ||
        values.count(L"sha256") == 0 || values.count(L"size") == 0) {
        error = L"更新清单字段不完整或包含未知字段。";
        return false;
    }
    if (!IsVersion(values[L"version"])) {
        error = L"更新版本号格式无效。";
        return false;
    }
    if (!IsHttpsUrl(values[L"url"])) {
        error = L"更新下载地址必须使用 HTTPS。";
        return false;
    }
    std::wstring sha = values[L"sha256"];
    if (sha.size() != 64 || !std::all_of(sha.begin(), sha.end(), [](wchar_t ch) {
            return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') ||
                   (ch >= L'A' && ch <= L'F');
        })) {
        error = L"更新 SHA-256 格式无效。";
        return false;
    }
    std::string sizeText;
    sizeText.reserve(values[L"size"].size());
    for (const wchar_t ch : values[L"size"]) {
        if (ch < L'0' || ch > L'9') {
            error = L"更新文件大小无效。";
            return false;
        }
        sizeText.push_back(static_cast<char>(ch));
    }
    std::uint64_t size = 0;
    const auto parsed = std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), size);
    if (parsed.ec != std::errc{} || parsed.ptr != sizeText.data() + sizeText.size() ||
        size == 0 || size > kMaximumExecutableBytes) {
        error = L"更新文件大小无效。";
        return false;
    }
    std::transform(sha.begin(), sha.end(), sha.begin(), towupper);
    manifest.version = values[L"version"];
    manifest.url = values[L"url"];
    manifest.sha256 = std::move(sha);
    manifest.size = size;
    error.clear();
    return true;
}

std::wstring Sha256Hex(const std::vector<std::uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD returned = 0;
    std::vector<std::uint8_t> object;
    std::vector<std::uint8_t> digest;
    std::wstring result;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &returned, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &returned, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    object.resize(objectLength);
    digest.resize(hashLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) >= 0 &&
        (bytes.empty() || BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
                                         static_cast<ULONG>(bytes.size()), 0) >= 0) &&
        BCryptFinishHash(hash, digest.data(), hashLength, 0) >= 0) {
        result = Hex(digest.data(), digest.size());
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

bool VerifyUpdatePayload(const std::vector<std::uint8_t>& bytes,
                         const UpdateManifest& manifest, std::wstring& error) {
    if (bytes.size() != manifest.size) {
        error = L"更新文件大小与清单不一致。";
        return false;
    }
    const std::wstring actual = Sha256Hex(bytes);
    if (actual.empty() || _wcsicmp(actual.c_str(), manifest.sha256.c_str()) != 0) {
        error = L"更新文件 SHA-256 校验失败。";
        return false;
    }
    error.clear();
    return true;
}

bool SelectUpdateManifest(const UpdateManifest* gitee, const UpdateManifest* github,
                          const std::wstring& currentVersion, UpdateManifest& selected) {
    const UpdateManifest* primary = nullptr;
    const UpdateManifest* alternate = nullptr;
    if (gitee) primary = gitee;
    if (github && (!primary || util::IsNewerVersion(github->version, primary->version))) {
        alternate = primary;
        primary = github;
    } else if (github) {
        alternate = github;
    }
    if (!primary || !util::IsNewerVersion(primary->version, currentVersion)) return false;

    selected = *primary;
    selected.fallbackUrl.clear();
    selected.fallbackSha256.clear();
    selected.fallbackSize = 0;
    if (alternate && alternate->version == primary->version) {
        selected.fallbackUrl = alternate->url;
        selected.fallbackSha256 = alternate->sha256;
        selected.fallbackSize = alternate->size;
    }
    return true;
}

UpdateChecker::~UpdateChecker() {
    Stop();
}

void UpdateChecker::Start(CheckCallback callback) {
    Stop();
    auto state = std::make_shared<State>();
    state->job = Job::Check;
    state->checkCallback = std::move(callback);
    state->downloadCallback = {};
    state->manifest = {};
    state->destination.clear();
    ResetEvent(state->workerExited);
    state_ = state;
    try {
        thread_ = std::thread(&UpdateChecker::Run, state);
    } catch (...) {
        state->stopping.store(true);
        state_ = std::make_shared<State>();
    }
}

void UpdateChecker::Download(UpdateManifest manifest, std::wstring destination,
                             DownloadCallback callback) {
    Stop();
    auto state = std::make_shared<State>();
    state->job = Job::Download;
    state->checkCallback = {};
    state->downloadCallback = std::move(callback);
    state->manifest = std::move(manifest);
    state->destination = std::move(destination);
    ResetEvent(state->workerExited);
    state_ = state;
    try {
        thread_ = std::thread(&UpdateChecker::Run, state);
    } catch (...) {
        state->stopping.store(true);
        state_ = std::make_shared<State>();
    }
}

void UpdateChecker::Stop() {
    const auto state = state_;
    if (state) {
        state->stopping.store(true);
        if (const HINTERNET request = state->request.exchange(nullptr)) WinHttpCloseHandle(request);
        if (const HINTERNET session = state->session.exchange(nullptr)) WinHttpCloseHandle(session);
    }
    if (thread_.joinable()) {
        const DWORD wait = state && state->workerExited
            ? WaitForSingleObject(state->workerExited, 2500) : WAIT_OBJECT_0;
        if (wait == WAIT_OBJECT_0) thread_.join();
        else thread_.detach();
    }
    if (state) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->checkCallback = {};
        state->downloadCallback = {};
    }
    state_ = std::make_shared<State>();
}

bool UpdateChecker::FetchUrl(const std::shared_ptr<State>& state, const std::wstring& url, std::size_t maximumBytes,
                             std::vector<std::uint8_t>& content, std::wstring& error) {
    content.clear();
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components) || components.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"更新地址无效。";
        return false;
    }
    const HINTERNET session = state->session.load();
    if (!session || state->stopping.load()) return false;
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (!connection) {
        error = L"无法连接更新服务器。";
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connection);
        error = L"无法创建更新请求。";
        return false;
    }
    state->request.store(request);
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    bool ok = false;
    if (!state->stopping.load() &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD length = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            error = L"更新服务器返回 HTTP " + std::to_wstring(status) + L"。";
        } else {
            ok = true;
            while (!state->stopping.load()) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) {
                    ok = false;
                    error = L"读取更新数据失败。";
                    break;
                }
                if (available == 0) break;
                if (content.size() > maximumBytes || available > maximumBytes - content.size()) {
                    ok = false;
                    error = L"更新数据超过大小限制。";
                    break;
                }
                const std::size_t oldSize = content.size();
                content.resize(oldSize + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, content.data() + oldSize, available, &read) || read == 0) {
                    content.resize(oldSize);
                    ok = false;
                    error = L"读取更新数据失败。";
                    break;
                }
                content.resize(oldSize + read);
            }
        }
    } else if (!state->stopping.load()) {
        error = L"更新请求失败。";
    }
    if (state->request.exchange(nullptr) == request) WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    if (state->stopping.load()) return false;
    return ok;
}

void UpdateChecker::Run(const std::shared_ptr<State>& state) {
    const std::wstring userAgent = L"SerialMate/" + std::wstring(version::kCurrent);
    const HINTERNET session = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    state->session.store(session);
    if (!session) { SetEvent(state->workerExited); return; }
    WinHttpSetTimeouts(session, 900, 900, 1200, 1200);
    if (state->stopping.load()) {
        if (const HINTERNET active = state->session.exchange(nullptr)) WinHttpCloseHandle(active);
        SetEvent(state->workerExited);
        return;
    }

    Job job;
    CheckCallback checkCallback;
    DownloadCallback downloadCallback;
    UpdateManifest manifest;
    std::wstring destination;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        job = state->job;
        checkCallback = state->checkCallback;
        downloadCallback = state->downloadCallback;
        manifest = state->manifest;
        destination = state->destination;
    }

    if (job == Job::Check) {
        const std::array<std::wstring, 2> urls = {
            L"https://gitee.com/" + std::wstring(version::kGiteeOwner) + L"/" +
                std::wstring(version::kGiteeRepository) + L"/raw/main/" + std::wstring(version::kGiteeManifestName),
                L"https://raw.githubusercontent.com/" + std::wstring(version::kGitHubOwner) + L"/" +
                std::wstring(version::kGitHubRepository) + L"/main/" + std::wstring(version::kGitHubManifestName)};
        std::array<UpdateManifest, 2> candidates{};
        std::array<bool, 2> valid{};
        for (std::size_t index = 0; index < urls.size(); ++index) {
            std::vector<std::uint8_t> bytes;
            std::wstring error;
            UpdateManifest candidate;
            if (FetchUrl(state, urls[index], kMaximumManifestBytes, bytes, error) &&
                ParseUpdateManifest(bytes, candidate, error)) {
                candidates[index] = std::move(candidate);
                valid[index] = true;
            }
            if (state->stopping.load()) break;
        }
        if (!state->stopping.load()) {
            UpdateManifest selected;
            const UpdateManifest* gitee = valid[0] ? &candidates[0] : nullptr;
            const UpdateManifest* github = valid[1] ? &candidates[1] : nullptr;
            if (SelectUpdateManifest(gitee, github, version::kCurrent, selected) && checkCallback)
                checkCallback(std::move(selected));
        }
    } else {
        UpdateDownloadResult result;
        result.manifest = manifest;
        result.path = destination;
        std::vector<std::uint8_t> bytes;
        if (manifest.size == 0 || manifest.size > kMaximumExecutableBytes) {
            result.error = L"更新文件大小无效。";
        } else {
            auto tryDownload = [&](const UpdateManifest& candidate) {
                bytes.clear();
                std::wstring error;
                if (!FetchUrl(state, candidate.url, static_cast<std::size_t>(candidate.size), bytes, error) ||
                    !VerifyUpdatePayload(bytes, candidate, error)) {
                    result.error = std::move(error);
                    return false;
                }
                result.manifest = candidate;
                return true;
            };
            bool downloaded = tryDownload(manifest);
            if (!downloaded && !state->stopping.load() && !manifest.fallbackUrl.empty()) {
                UpdateManifest fallback = manifest;
                fallback.url = manifest.fallbackUrl;
                fallback.sha256 = manifest.fallbackSha256;
                fallback.size = manifest.fallbackSize;
                fallback.fallbackUrl.clear();
                fallback.fallbackSha256.clear();
                fallback.fallbackSize = 0;
                downloaded = tryDownload(fallback);
            }
            if (downloaded) {
            try {
                const std::filesystem::path path(destination);
                std::filesystem::create_directories(path.parent_path());
                const std::filesystem::path temporary = path.wstring() + L".tmp";
                {
                    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                    output.write(reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<std::streamsize>(bytes.size()));
                    if (!output) throw std::runtime_error("write failed");
                }
                std::error_code ec;
                std::filesystem::remove(path, ec);
                ec.clear();
                std::filesystem::rename(temporary, path, ec);
                if (ec) {
                    std::filesystem::remove(temporary);
                    result.error = L"无法保存已校验的更新文件。";
                } else {
                    result.success = true;
                }
            } catch (...) {
                result.error = L"无法保存已校验的更新文件。";
            }
            }
        }
        if (!state->stopping.load() && downloadCallback) downloadCallback(std::move(result));
    }
    if (const HINTERNET active = state->session.exchange(nullptr)) WinHttpCloseHandle(active);
    SetEvent(state->workerExited);
}
