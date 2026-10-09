#include "vless_client.h"

#include "schannel_tls.h"
#include "huff-tables.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
struct VlessConfig {
    std::array<unsigned char, 16> id{};
    std::wstring endpoint;
    unsigned short port{};
    std::wstring host;
    std::wstring path;
};

int hexValue(wchar_t value) {
    if (value >= L'0' && value <= L'9') return value - L'0';
    if (value >= L'a' && value <= L'f') return value - L'a' + 10;
    if (value >= L'A' && value <= L'F') return value - L'A' + 10;
    return -1;
}

bool parseUuid(std::wstring_view value, std::array<unsigned char, 16>& output) {
    std::array<int, 32> digits{};
    size_t count = 0;
    for (wchar_t c : value) {
        if (c == L'-') continue;
        int digit = hexValue(c);
        if (digit < 0 || count >= digits.size()) return false;
        digits[count++] = digit;
    }
    if (count != digits.size()) return false;
    for (size_t i = 0; i < output.size(); ++i)
        output[i] = static_cast<unsigned char>((digits[i * 2] << 4) | digits[i * 2 + 1]);
    return true;
}

std::wstring urlDecode(std::wstring_view value) {
    std::string bytes;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'%' && i + 2 < value.size()) {
            int high = hexValue(value[i + 1]), low = hexValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                bytes.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        if (value[i] == L'+') bytes.push_back(' ');
        else if (value[i] <= 0x7f) bytes.push_back(static_cast<char>(value[i]));
        else {
            int needed = WideCharToMultiByte(CP_UTF8, 0, &value[i], 1, nullptr, 0, nullptr, nullptr);
            size_t offset = bytes.size();
            bytes.resize(offset + static_cast<size_t>(needed));
            WideCharToMultiByte(CP_UTF8, 0, &value[i], 1, bytes.data() + offset, needed, nullptr, nullptr);
        }
    }
    if (bytes.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), result.data(), size);
    return result;
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::unordered_map<std::wstring, std::wstring> parseQuery(std::wstring_view query) {
    std::unordered_map<std::wstring, std::wstring> result;
    size_t begin = 0;
    while (begin <= query.size()) {
        size_t end = query.find(L'&', begin);
        auto item = query.substr(begin, end == std::wstring_view::npos ? query.size() - begin : end - begin);
        size_t equals = item.find(L'=');
        auto key = urlDecode(item.substr(0, equals));
        auto value = equals == std::wstring_view::npos ? std::wstring{} : urlDecode(item.substr(equals + 1));
        std::transform(key.begin(), key.end(), key.begin(), towlower);
        result[std::move(key)] = std::move(value);
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    }
    return result;
}

bool parseProfile(const std::wstring& uri, VlessConfig& config, std::wstring& error) {
    constexpr std::wstring_view prefix = L"vless://";
    if (uri.rfind(prefix, 0) != 0) { error = L"Некорректная VLESS-ссылка"; return false; }
    size_t at = uri.find(L'@', prefix.size());
    size_t queryAt = uri.find(L'?', at == std::wstring::npos ? prefix.size() : at);
    if (at == std::wstring::npos || queryAt == std::wstring::npos ||
        !parseUuid(std::wstring_view(uri).substr(prefix.size(), at - prefix.size()), config.id)) {
        error = L"В VLESS-ссылке нет корректного UUID или параметров";
        return false;
    }
    auto authority = std::wstring_view(uri).substr(at + 1, queryAt - at - 1);
    size_t colon = authority.rfind(L':');
    if (colon == std::wstring_view::npos) { error = L"В VLESS-ссылке не указан порт"; return false; }
    config.endpoint.assign(authority.substr(0, colon));
    wchar_t* portEnd{};
    unsigned long port = wcstoul(std::wstring(authority.substr(colon + 1)).c_str(), &portEnd, 10);
    if (!portEnd || *portEnd || port == 0 || port > 65535) { error = L"Некорректный порт VLESS"; return false; }
    config.port = static_cast<unsigned short>(port);
    size_t hash = uri.find(L'#', queryAt + 1);
    auto params = parseQuery(std::wstring_view(uri).substr(queryAt + 1,
        (hash == std::wstring::npos ? uri.size() : hash) - queryAt - 1));
    std::wstring type = params[L"type"], security = params[L"security"];
    std::transform(type.begin(), type.end(), type.begin(), towlower);
    std::transform(security.begin(), security.end(), security.begin(), towlower);
    if (type != L"xhttp") { error = L"Сейчас нативный VLESS поддерживает профиль XHTTP-CDN"; return false; }
    if (security != L"tls") { error = L"VLESS XHTTP пока поддерживает обычный TLS, без REALITY"; return false; }
    config.host = params[L"host"].empty() ? config.endpoint : params[L"host"];
    config.path = params[L"path"].empty() ? L"/" : params[L"path"];
    if (config.path.front() != L'/') config.path.insert(config.path.begin(), L'/');
    if (config.path.back() != L'/') config.path.push_back(L'/');
    return true;
}

std::wstring newSessionId() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return std::to_wstring(GetTickCount64());
    wchar_t value[40]{};
    StringFromGUID2(id, value, static_cast<int>(std::size(value)));
    std::wstring result(value + 1, value + 37);
    std::transform(result.begin(), result.end(), result.begin(), towlower);
    return result;
}

size_t hpackHuffmanBytes(std::string_view value) {
    size_t bits = 0;
    for (unsigned char character : value) bits += encode_table[character].bits;
    return (bits + 7) / 8;
}

bool secureRandom(unsigned char* output, size_t length) {
    return length <= ULONG_MAX && BCryptGenRandom(nullptr, output, static_cast<ULONG>(length),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::string tokenishPadding(int targetHuffmanBytes) {
    static constexpr char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    size_t length = std::max<size_t>(1, (static_cast<size_t>(targetHuffmanBytes) * 5 + 3) / 4);
    std::string result;
    result.reserve(length + 4);
    std::array<unsigned char, 256> random{};
    while (result.size() < length) {
        if (!secureRandom(random.data(), random.size())) return std::string(static_cast<size_t>(targetHuffmanBytes), 'X');
        for (unsigned char value : random) {
            if (value >= 248) continue;
            result.push_back(alphabet[value % 62]);
            if (result.size() == length) break;
        }
    }
    char adjustment = 'X';
    for (int iteration = 0; iteration < 150; ++iteration) {
        int difference = static_cast<int>(hpackHuffmanBytes(result)) - targetHuffmanBytes;
        if (difference >= -2 && difference <= 2) return result;
        if (difference < 0) {
            result.push_back(adjustment);
            adjustment = adjustment == 'X' ? 'Z' : 'X';
        } else if (result.size() > 1) result.pop_back();
        else break;
    }
    return result;
}

std::string paddingValue(const VlessConfig& config, std::string_view path) {
    std::array<unsigned char, 4> random{};
    unsigned value = secureRandom(random.data(), random.size())
        ? (static_cast<unsigned>(random[0]) | static_cast<unsigned>(random[1]) << 8 |
            static_cast<unsigned>(random[2]) << 16 | static_cast<unsigned>(random[3]) << 24)
        : GetTickCount();
    int targetLength = 100 + static_cast<int>(value % 901);
    std::string result = "https://" + utf8(config.host) + std::string(path);
    result += path.find('?') == std::string_view::npos ? "?_dc=" : "&_dc=";
    result += tokenishPadding(targetLength);
    return result;
}

std::vector<std::pair<std::string, std::string>> browserFetchHeaders() {
    // Mirrors Xray-core v26.3.27 common/utils.TryDefaultHeadersWith("fetch").
    return {
        {"sec-ch-ua", "\"Google Chrome\";v=\"149\", \"Chromium\";v=\"149\", \"Not)A;Brand\";v=\"24\""},
        {"sec-ch-ua-mobile", "?0"},
        {"sec-ch-ua-platform", "\"Windows\""},
        {"dnt", "1"},
        {"user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36"},
        {"accept-language", "en-US,en;q=0.9"},
        {"sec-fetch-mode", "cors"},
        {"sec-fetch-dest", "empty"},
        {"sec-fetch-site", "same-origin"},
        {"priority", "u=1, i"},
        {"cache-control", "no-cache"},
        {"pragma", "no-cache"},
        {"accept", "*/*"},
        {"accept-encoding", "gzip"},
    };
}

bool sendAll(SOCKET socket, const unsigned char* data, size_t length) {
    while (length) {
        int sent = send(socket, reinterpret_cast<const char*>(data),
            static_cast<int>(std::min<size_t>(length, INT_MAX)), 0);
        if (sent <= 0) return false;
        data += sent;
        length -= static_cast<size_t>(sent);
    }
    return true;
}

bool splitDestination(const std::string& destination, std::string& host, unsigned short& port) {
    size_t colon = destination.rfind(':');
    if (colon == std::string::npos) return false;
    host = destination.substr(0, colon);
    if (host.size() > 1 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    char* end{};
    unsigned long value = strtoul(destination.c_str() + colon + 1, &end, 10);
    if (!end || *end || value == 0 || value > 65535) return false;
    port = static_cast<unsigned short>(value);
    return !host.empty();
}

bool makeVlessHeader(const VlessConfig& config, const std::string& destination,
    std::vector<unsigned char>& header, std::wstring& error, unsigned char command = 1) {
    std::string host;
    unsigned short port{};
    if (!splitDestination(destination, host, port)) { error = L"VLESS получил некорректный адрес назначения"; return false; }
    header.reserve(40 + host.size());
    header.push_back(0);
    header.insert(header.end(), config.id.begin(), config.id.end());
    header.push_back(0);
    header.push_back(command);
    header.push_back(static_cast<unsigned char>(port >> 8));
    header.push_back(static_cast<unsigned char>(port));
    in_addr ipv4{};
    in6_addr ipv6{};
    if (inet_pton(AF_INET, host.c_str(), &ipv4) == 1) {
        header.push_back(1);
        auto* bytes = reinterpret_cast<const unsigned char*>(&ipv4);
        header.insert(header.end(), bytes, bytes + sizeof(ipv4));
    } else if (inet_pton(AF_INET6, host.c_str(), &ipv6) == 1) {
        header.push_back(3);
        auto* bytes = reinterpret_cast<const unsigned char*>(&ipv6);
        header.insert(header.end(), bytes, bytes + sizeof(ipv6));
    } else {
        if (host.size() > 255) { error = L"Слишком длинное доменное имя для VLESS"; return false; }
        header.push_back(2);
        header.push_back(static_cast<unsigned char>(host.size()));
        header.insert(header.end(), host.begin(), host.end());
    }
    return true;
}

std::vector<unsigned char> makeFirstUpload(std::span<const unsigned char> vlessHeader,
    std::span<const unsigned char> applicationData) {
    std::vector<unsigned char> upload;
    upload.reserve(vlessHeader.size() + applicationData.size());
    upload.insert(upload.end(), vlessHeader.begin(), vlessHeader.end());
    upload.insert(upload.end(), applicationData.begin(), applicationData.end());
    return upload;
}

nghttp2_nv header(std::string_view name, std::string_view value) {
    return {reinterpret_cast<uint8_t*>(const_cast<char*>(name.data())),
        reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())), name.size(), value.size(),
        NGHTTP2_NV_FLAG_NONE};
}

class H2Connection {
public:
    explicit H2Connection(const VlessConfig& config) : config_(config) {}
    ~H2Connection() { stop(); }

    bool connect(std::wstring& error) {
#ifdef BIG_HEAD_VPN_TESTING
        fwprintf(stderr, L"h2_connect tls begin host=%ls:%u\n", config_.endpoint.c_str(), config_.port); fflush(stderr);
#endif
        if (!tls_.connect(config_.endpoint, config_.port, error)) return false;
#ifdef BIG_HEAD_VPN_TESTING
        fwprintf(stderr, L"h2_connect tls ready\n"); fflush(stderr);
#endif
        nghttp2_session_callbacks* callbacks{};
        if (nghttp2_session_callbacks_new(&callbacks) != 0) { error = L"VLESS: nghttp2 callbacks"; return false; }
        nghttp2_session_callbacks_set_send_callback(callbacks, sendCallback);
        nghttp2_session_callbacks_set_on_header_callback(callbacks, headerCallback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, dataCallback);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, closeCallback);
        int result = nghttp2_session_client_new(&session_, callbacks, this);
        nghttp2_session_callbacks_del(callbacks);
        if (result != 0) { error = h2Error(L"создание HTTP/2-сеанса", result); return false; }
        // Match the Go HTTP/2 client used by Xray-core v26.3.27.  Besides
        // protocol parity, the larger stream window prevents a busy
        // stream-down from stalling behind the default 64 KiB window.
        nghttp2_settings_entry settings[]{
            {NGHTTP2_SETTINGS_ENABLE_PUSH, 0},
            {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 4U * 1024U * 1024U},
            {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, 16384U},
            {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, 10U * 1024U * 1024U},
        };
        result = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings, std::size(settings));
        if (result == 0)
            result = nghttp2_submit_window_update(session_, NGHTTP2_FLAG_NONE, 0, 1073741824);
#ifdef BIG_HEAD_VPN_TESTING
        fwprintf(stderr, L"h2_connect settings=%d send begin\n", result); fflush(stderr);
#endif
        if (result != 0 || nghttp2_session_send(session_) != 0) {
            error = h2Error(L"отправка HTTP/2 preface", result);
            return false;
        }
#ifdef BIG_HEAD_VPN_TESTING
        fwprintf(stderr, L"h2_connect preface sent\n"); fflush(stderr);
#endif
        reader_ = std::thread([this] { readLoop(); });
        keepalive_ = std::thread([this] { keepaliveLoop(); });
        return true;
    }

    int submitDownload(const std::string& path, SOCKET local, std::wstring& error) {
        return submit(path, {}, true, local, {}, false, error);
    }

    int submitUdpDownload(const std::string& path,
        std::function<void(std::vector<unsigned char>)> packetHandler, std::wstring& error) {
        return submit(path, {}, true, INVALID_SOCKET, std::move(packetHandler), false, error);
    }

    int submitUpload(const std::string& path, std::span<const unsigned char> data, std::wstring& error) {
        return submit(path, data, false, INVALID_SOCKET, {}, false, error);
    }

    int submitUploadAsync(const std::string& path, std::span<const unsigned char> data, std::wstring& error) {
        return submit(path, data, false, INVALID_SOCKET, {}, true, error);
    }

    bool waitHeaders(int streamId, std::wstring& error) {
        std::unique_lock lock(mutex_);
        bool ready = changed_.wait_for(lock, std::chrono::seconds(15), [&] {
            auto it = streams_.find(streamId);
            return failed_ || it == streams_.end() || it->second->headersDone || it->second->closed;
        });
        if (!ready) { error = L"VLESS XHTTP: сервер не ответил за 15 секунд"; return false; }
        if (failed_) { error = failure_; return false; }
        auto it = streams_.find(streamId);
        if (it == streams_.end()) { error = L"VLESS XHTTP: поток HTTP/2 исчез"; return false; }
        if (it->second->status != 200) {
            error = L"VLESS XHTTP вернул HTTP " + std::to_wstring(it->second->status);
            return false;
        }
        it->second->observed = true;
        return true;
    }

    std::wstring downloadFailure(int streamId) {
        std::lock_guard lock(mutex_);
        auto it = streams_.find(streamId);
        if (it == streams_.end()) return L"VLESS XHTTP: stream-down исчез";
        it->second->observed = true;
        return it->second->prematureDownloadClose
            ? L"VLESS XHTTP: сервер закрыл stream-down без ответа" : std::wstring{};
    }

    bool streamOpen(int streamId) const {
        std::lock_guard lock(mutex_);
        auto it = streams_.find(streamId);
        return it != streams_.end() && !it->second->closed;
    }

    void cancel(int streamId) {
        std::lock_guard lock(mutex_);
        if (!session_ || stopping_) return;
        auto it = streams_.find(streamId);
        if (it == streams_.end() || it->second->closed) return;
        nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, streamId, NGHTTP2_CANCEL);
        nghttp2_session_send(session_);
    }

    void stop() {
        if (stopping_.exchange(true)) return;
        // A sender can hold mutex_ while blocked in TLS I/O.
        tls_.shutdownTransport();
        {
            std::lock_guard lock(mutex_);
            if (!failed_) {
                failed_ = true;
                failure_ = L"VLESS остановлен";
            }
        }
        changed_.notify_all();
        // Abort socket I/O before joining.  The keepalive thread can be
        // inside a blocking send for up to SO_SNDTIMEO, which otherwise
        // makes an ordinary disconnect take many seconds.
        tls_.shutdownTransport();
        if (keepalive_.joinable()) keepalive_.join();
        {
            std::lock_guard lock(mutex_);
            for (auto& [id, stream] : streams_) {
                (void)id;
                if (stream->download && stream->local != INVALID_SOCKET) shutdown(stream->local, SD_BOTH);
            }
        }
        // Schannel state remains alive until the reader exits, so
        // DecryptMessage cannot race DeleteSecurityContext.
        if (reader_.joinable()) reader_.join();
        if (session_) { nghttp2_session_del(session_); session_ = nullptr; }
        tls_.close();
    }

    std::wstring failure() const {
        std::lock_guard lock(mutex_);
        return failure_;
    }

private:
    struct Stream {
        std::vector<unsigned char> upload;
        size_t uploadOffset{};
        std::vector<unsigned char> pending;
        int status{};
        bool download{};
        bool headersDone{};
        bool closed{};
        bool vlessHeaderDone{};
        bool observed{};
        bool prematureDownloadClose{};
        SOCKET local{INVALID_SOCKET};
        std::function<void(std::vector<unsigned char>)> udpPacketHandler;
    };

    static std::wstring h2Error(const wchar_t* action, int code) {
        return L"VLESS HTTP/2: " + std::wstring(action) + L": " +
            std::wstring(nghttp2_strerror(code), nghttp2_strerror(code) + strlen(nghttp2_strerror(code)));
    }

    void fail(std::wstring message) {
        {
            std::lock_guard lock(mutex_);
            if (!failed_) { failed_ = true; failure_ = std::move(message); }
            for (auto& [id, stream] : streams_) {
                (void)id;
                if (stream->download && stream->local != INVALID_SOCKET) shutdown(stream->local, SD_BOTH);
            }
        }
        changed_.notify_all();
    }

    int submit(const std::string& path, std::span<const unsigned char> data, bool download, SOCKET local,
        std::function<void(std::vector<unsigned char>)> udpPacketHandler, bool autoDiscard,
        std::wstring& error) {
        std::string authority = utf8(config_.host);
        // Xray applies padding before session/sequence metadata is appended.
        std::string padding = paddingValue(config_, utf8(config_.path));
        std::string contentLength = std::to_string(data.size());
        auto values = browserFetchHeaders();
        values.insert(values.begin(), {{":method", "GET"}, {":scheme", "https"},
            {":authority", authority}, {":path", path}});
        values.emplace_back("x-cache", padding);
        if (!download) values.emplace_back("content-length", contentLength);
        std::vector<nghttp2_nv> headers;
        headers.reserve(values.size());
        for (auto& [name, value] : values) headers.push_back(header(name, value));
        auto stream = std::make_unique<Stream>();
        stream->download = download;
        stream->local = local;
        stream->udpPacketHandler = std::move(udpPacketHandler);
        stream->observed = autoDiscard;
        stream->upload.assign(data.begin(), data.end());
        nghttp2_data_provider provider{};
        nghttp2_data_provider* providerPtr = nullptr;
        if (!download) {
            provider.source.ptr = stream.get();
            provider.read_callback = uploadCallback;
            providerPtr = &provider;
        }
        std::lock_guard lock(mutex_);
        if (failed_ || stopping_) { error = failure_.empty() ? L"VLESS остановлен" : failure_; return -1; }
        for (auto it = streams_.begin(); it != streams_.end();) {
            if (it->second->closed && it->second->observed) it = streams_.erase(it);
            else ++it;
        }
        int streamId = nghttp2_submit_request(session_, nullptr, headers.data(), headers.size(), providerPtr, nullptr);
        if (streamId < 0) { error = h2Error(L"создание запроса", streamId); return -1; }
        streams_.emplace(streamId, std::move(stream));
        int result = nghttp2_session_send(session_);
        if (result != 0) { error = h2Error(L"отправка запроса", result); return -1; }
#ifdef BIG_HEAD_VPN_TESTING
        fwprintf(stderr, L"h2_submit stream=%d kind=%ls bytes=%zu path=%hs\n", streamId,
            download ? L"down" : L"up", data.size(), path.c_str());
        fflush(stderr);
#endif
        return streamId;
    }

    void readLoop() {
        while (!stopping_) {
            std::vector<unsigned char> data;
            std::wstring error;
            if (!tls_.read(data, error)) {
                if (!stopping_) fail(std::move(error));
                return;
            }
            std::lock_guard lock(mutex_);
            ssize_t result = nghttp2_session_mem_recv(session_, data.data(), data.size());
            if (result < 0) {
                failed_ = true;
                failure_ = h2Error(L"разбор ответа", static_cast<int>(result));
                changed_.notify_all();
                for (auto& [id, stream] : streams_) {
                    (void)id;
                    if (stream->download && stream->local != INVALID_SOCKET) shutdown(stream->local, SD_BOTH);
                }
                return;
            }
            int sent = nghttp2_session_send(session_);
            if (sent != 0) {
                failed_ = true;
                failure_ = h2Error(L"служебный кадр", sent);
                changed_.notify_all();
                for (auto& [id, stream] : streams_) {
                    (void)id;
                    if (stream->download && stream->local != INVALID_SOCKET) shutdown(stream->local, SD_BOTH);
                }
                return;
            }
        }
    }

    void keepaliveLoop() {
        for (;;) {
            std::unique_lock lock(mutex_);
            if (changed_.wait_for(lock, std::chrono::seconds(8), [&] { return stopping_ || failed_; })) return;
            std::array<uint8_t, 8> opaque{};
            unsigned long long tick = GetTickCount64();
            for (size_t i = 0; i < opaque.size(); ++i)
                opaque[i] = static_cast<uint8_t>(tick >> (i * 8U));
            int result = nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, opaque.data());
            if (result == 0) result = nghttp2_session_send(session_);
            if (result != 0) {
                failed_ = true;
                failure_ = h2Error(L"HTTP/2 keepalive", result);
                for (auto& [id, stream] : streams_) {
                    (void)id;
                    if (stream->download && stream->local != INVALID_SOCKET) shutdown(stream->local, SD_BOTH);
                }
                changed_.notify_all();
                return;
            }
#ifdef BIG_HEAD_VPN_TESTING
            fwprintf(stderr, L"h2_keepalive ping\n");
            fflush(stderr);
#endif
        }
    }

    static ssize_t sendCallback(nghttp2_session*, const uint8_t* data, size_t length, int, void* userData) {
        auto& self = *static_cast<H2Connection*>(userData);
        std::wstring error;
        if (!self.tls_.write(std::span<const unsigned char>(data, length), error)) {
            self.failure_ = std::move(error);
            self.failed_ = true;
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        return static_cast<ssize_t>(length);
    }

    static ssize_t uploadCallback(nghttp2_session*, int32_t, uint8_t* buffer, size_t length,
        uint32_t* flags, nghttp2_data_source* source, void*) {
        auto& stream = *static_cast<Stream*>(source->ptr);
        size_t available = stream.upload.size() - stream.uploadOffset;
        size_t copied = std::min(length, available);
        if (copied) {
            std::copy_n(stream.upload.data() + stream.uploadOffset, copied, buffer);
            stream.uploadOffset += copied;
        }
        if (stream.uploadOffset == stream.upload.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
#ifdef BIG_HEAD_VPN_TESTING
        fwprintf(stderr, L"h2_upload_data copied=%zu total=%zu eof=%d\n", copied,
            stream.upload.size(), stream.uploadOffset == stream.upload.size());
        fflush(stderr);
#endif
        return static_cast<ssize_t>(copied);
    }

    static int headerCallback(nghttp2_session*, const nghttp2_frame* frame,
        const uint8_t* name, size_t nameLength, const uint8_t* value, size_t valueLength,
        uint8_t, void* userData) {
        if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_RESPONSE) return 0;
        auto& self = *static_cast<H2Connection*>(userData);
        auto it = self.streams_.find(frame->hd.stream_id);
        if (it == self.streams_.end()) return 0;
        if (nameLength == 7 && memcmp(name, ":status", 7) == 0) {
            std::string text(reinterpret_cast<const char*>(value), valueLength);
            it->second->status = atoi(text.c_str());
            it->second->headersDone = true;
            if (it->second->download && it->second->status != 200 && !self.failed_) {
                self.failed_ = true;
                self.failure_ = L"VLESS XHTTP stream-down вернул HTTP " +
                    std::to_wstring(it->second->status);
                if (it->second->local != INVALID_SOCKET) shutdown(it->second->local, SD_BOTH);
            }
#ifdef BIG_HEAD_VPN_TESTING
            fwprintf(stderr, L"h2_status stream=%d status=%d\n", frame->hd.stream_id, it->second->status);
            fflush(stderr);
#endif
            self.changed_.notify_all();
        }
        if (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) {
            it->second->headersDone = true;
            self.changed_.notify_all();
        }
        return 0;
    }

    static int dataCallback(nghttp2_session* session, uint8_t, int32_t streamId,
        const uint8_t* data, size_t length, void* userData) {
        auto& self = *static_cast<H2Connection*>(userData);
        auto it = self.streams_.find(streamId);
        if (it == self.streams_.end() || !it->second->download) return 0;
#ifdef BIG_HEAD_VPN_TESTING
        fwprintf(stderr, L"h2_download_data stream=%d bytes=%zu first=%u\n", streamId, length,
            length ? static_cast<unsigned>(data[0]) : 0U);
        fflush(stderr);
#endif
        Stream& stream = *it->second;
        stream.pending.insert(stream.pending.end(), data, data + length);
        if (!stream.vlessHeaderDone) {
            if (stream.pending.size() < 2 ||
                stream.pending.size() < static_cast<size_t>(2 + stream.pending[1])) return 0;
            if (stream.pending[0] != 0) return NGHTTP2_ERR_CALLBACK_FAILURE;
            stream.pending.erase(stream.pending.begin(), stream.pending.begin() + 2 + stream.pending[1]);
            stream.vlessHeaderDone = true;
        }
        if (stream.udpPacketHandler) {
            while (stream.pending.size() >= 2) {
                size_t packetLength = (static_cast<size_t>(stream.pending[0]) << 8U) | stream.pending[1];
                if (stream.pending.size() < packetLength + 2) break;
                if (packetLength) {
                    std::vector<unsigned char> packet(stream.pending.begin() + 2,
                        stream.pending.begin() + static_cast<std::ptrdiff_t>(packetLength + 2));
                    stream.udpPacketHandler(std::move(packet));
                }
                stream.pending.erase(stream.pending.begin(),
                    stream.pending.begin() + static_cast<std::ptrdiff_t>(packetLength + 2));
            }
        } else if (!stream.pending.empty()) {
            if (!sendAll(stream.local, stream.pending.data(), stream.pending.size())) {
                // Discord routinely closes speculative sockets while their
                // response is already in flight. Cancel only this stream;
                // returning CALLBACK_FAILURE would poison the shared XMUX.
                stream.pending.clear();
                stream.local = INVALID_SOCKET;
                nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, streamId, NGHTTP2_CANCEL);
                return 0;
            }
            stream.pending.clear();
        }
        return 0;
    }

    static int closeCallback(nghttp2_session*, int32_t streamId, uint32_t errorCode, void* userData) {
        (void)errorCode;
        auto& self = *static_cast<H2Connection*>(userData);
        auto it = self.streams_.find(streamId);
        if (it != self.streams_.end()) {
            it->second->closed = true;
            if (it->second->download && !self.stopping_) {
                if (errorCode == NGHTTP2_NO_ERROR && !it->second->vlessHeaderDone)
                    it->second->prematureDownloadClose = true;
                if (it->second->local != INVALID_SOCKET) shutdown(it->second->local, SD_BOTH);
            }
        }
#ifdef BIG_HEAD_VPN_TESTING
        fwprintf(stderr, L"h2_close stream=%d error=%u\n", streamId, errorCode);
        fflush(stderr);
#endif
        self.changed_.notify_all();
        return 0;
    }

    const VlessConfig& config_;
    SchannelTls tls_;
    nghttp2_session* session_{};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<int, std::unique_ptr<Stream>> streams_;
    std::thread reader_;
    std::thread keepalive_;
    std::atomic_bool stopping_{};
    bool failed_{};
    std::wstring failure_;
};
}

struct VlessClient::Impl {
    struct TcpConnectionSlot {
        std::shared_ptr<H2Connection> connection;
        unsigned usage{};
    };
    struct UdpSession {
        std::mutex mutex;
        std::string destination;
        std::string basePath;
        std::shared_ptr<H2Connection> transport;
        int download{-1};
        unsigned long long sequence{};
        std::atomic_ullong lastActivityTick{GetTickCount64()};
    };

    VlessConfig config;
    std::atomic_bool stopping{};
    std::mutex connectionMutex;
    std::condition_variable connectionChanged;
    std::shared_ptr<H2Connection> connection;
    std::shared_ptr<H2Connection> pendingConnection;
    std::vector<std::shared_ptr<TcpConnectionSlot>> tcpConnections;
    std::vector<std::shared_ptr<H2Connection>> pendingTcpConnections;
    bool connecting{};
    bool winsockStarted{};
    std::mutex udpMutex;
    std::unordered_map<uint32_t, std::shared_ptr<UdpSession>> udpSessions;
    UdpReceiveHandler udpHandler;
    ErrorHandler errorHandler;
    std::wstring lastReportedError;
    unsigned long long lastReportedErrorTick{};
    std::atomic_ullong udpSent{};
    std::atomic_ullong udpReceived{};

    void reportError(const std::wstring& error) {
        if (error.empty() || stopping) return;
        ErrorHandler handler;
        {
            std::lock_guard lock(udpMutex);
            unsigned long long now = GetTickCount64();
            if (error == lastReportedError && now - lastReportedErrorTick < 5000) return;
            lastReportedError = error;
            lastReportedErrorTick = now;
            handler = errorHandler;
        }
        if (handler) handler(error);
    }

    ~Impl() {
        stop();
        connection.reset(); pendingConnection.reset(); tcpConnections.clear(); pendingTcpConnections.clear();
        if (winsockStarted) WSACleanup();
    }

    void stop() {
        if (stopping.exchange(true)) return;
        std::vector<std::shared_ptr<H2Connection>> transports;
        {
            std::lock_guard lock(connectionMutex);
            if (connection) transports.push_back(connection);
            if (pendingConnection && pendingConnection != connection) transports.push_back(pendingConnection);
            for (const auto& slot : tcpConnections) if (slot->connection) transports.push_back(slot->connection);
            transports.insert(transports.end(), pendingTcpConnections.begin(), pendingTcpConnections.end());
        }
        for (const auto& transport : transports) if (transport) transport->stop();
        {
            std::lock_guard lock(udpMutex);
            udpSessions.clear();
            udpHandler = {};
        }
        connectionChanged.notify_all();
    }

    std::shared_ptr<TcpConnectionSlot> acquireTcpConnection(std::wstring& error) {
        std::shared_ptr<H2Connection> candidate;
        {
            std::lock_guard lock(connectionMutex);
            if (stopping) { error = L"VLESS уже остановлен"; return {}; }
            std::erase_if(tcpConnections, [](const auto& slot) {
                return slot->usage == 0 && !slot->connection->failure().empty();
            });
            for (const auto& slot : tcpConnections) {
                if (slot->usage == 0 && slot->connection->failure().empty()) {
                    ++slot->usage;
                    return slot;
                }
            }
            if (tcpConnections.size() >= 8) {
                auto found = std::min_element(tcpConnections.begin(), tcpConnections.end(), [](const auto& left, const auto& right) {
                    return left->usage < right->usage;
                });
                if (found != tcpConnections.end() && (*found)->connection->failure().empty()) {
                    ++(*found)->usage;
                    return *found;
                }
            }
            candidate = std::make_shared<H2Connection>(config);
            pendingTcpConnections.push_back(candidate);
        }
        bool connected = candidate->connect(error);
        std::shared_ptr<TcpConnectionSlot> slot;
        {
            std::lock_guard lock(connectionMutex);
            std::erase(pendingTcpConnections, candidate);
            if (connected && !stopping) {
                slot = std::make_shared<TcpConnectionSlot>();
                slot->connection = candidate;
                slot->usage = 1;
                tcpConnections.push_back(slot);
            }
        }
        if (!slot) {
            candidate->stop();
            if (error.empty()) error = L"VLESS уже остановлен";
        }
        return slot;
    }

    void releaseTcpConnection(const std::shared_ptr<TcpConnectionSlot>& slot) {
        std::lock_guard lock(connectionMutex);
        if (slot && slot->usage) --slot->usage;
    }

    std::shared_ptr<H2Connection> acquireConnection(std::wstring& error) {
        for (;;) {
            std::shared_ptr<H2Connection> candidate;
            {
                std::unique_lock lock(connectionMutex);
                if (stopping) { error = L"VLESS уже остановлен"; return {}; }
                if (connection && connection->failure().empty()) return connection;
                if (connecting) {
                    connectionChanged.wait(lock, [&] { return stopping || !connecting; });
                    continue;
                }
                connecting = true;
                candidate = std::make_shared<H2Connection>(config);
                pendingConnection = candidate;
            }
            bool connected = candidate->connect(error);
            {
                std::lock_guard lock(connectionMutex);
                if (connected && !stopping) connection = candidate;
                pendingConnection.reset();
                connecting = false;
            }
            connectionChanged.notify_all();
            if (connected && !stopping) return candidate;
            candidate->stop();
            if (error.empty()) error = L"VLESS уже остановлен";
            return {};
        }
    }

    bool relayTcp(const std::string& destination, SOCKET local, std::wstring& error, bool socksReply) {
#ifdef BIG_HEAD_VPN_TESTING
        fwprintf(stderr, L"vless_relay destination=%hs\n", destination.c_str());
        fflush(stderr);
#endif
        if (stopping) { error = L"VLESS уже остановлен"; return false; }
        std::vector<unsigned char> vlessHeader;
        if (!makeVlessHeader(config, destination, vlessHeader, error)) return false;
        auto slot = acquireTcpConnection(error);
        if (!slot) return false;
        struct LeaseGuard {
            Impl* owner;
            std::shared_ptr<TcpConnectionSlot> slot;
            ~LeaseGuard() { owner->releaseTcpConnection(slot); }
        } lease{this, slot};
        auto transport = slot->connection;
        std::string basePath = utf8(config.path) + utf8(newSessionId());
        int download = transport->submitDownload(basePath, local, error);
        if (download < 0) return false;
        struct DownloadGuard {
            H2Connection* connection;
            int stream;
            ~DownloadGuard() { connection->cancel(stream); }
        } downloadGuard{transport.get(), download};
        // Xray's SOCKS inbound confirms CONNECT as soon as the XHTTP
        // stream-down request has been opened.  That lets the application
        // produce (for example) its TLS ClientHello before packet-up /0.
        if (socksReply) {
            static constexpr unsigned char reply[]{5, 0, 0, 1, 0, 0, 0, 0, 0, 0};
            if (!sendAll(local, reply, sizeof(reply))) { error = L"Локальное приложение закрыло SOCKS5"; return false; }
        }
        std::array<unsigned char, 65536> buffer{};
        int received = recv(local, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (received <= 0) {
            if (!stopping) error = L"Локальное приложение закрыло соединение до отправки данных";
            return error.empty();
        }
        // packet-up mode in Xray buffers the VLESS request header and the
        // first application write into the same /0 request.  Sending a
        // header-only /0 changes server-side flushing behaviour on this CDN.
        std::vector<unsigned char> firstBody = makeFirstUpload(vlessHeader,
            std::span<const unsigned char>(buffer.data(), static_cast<size_t>(received)));
        int firstUpload = transport->submitUpload(basePath + "/0", firstBody, error);
        if (firstUpload < 0 || !transport->waitHeaders(firstUpload, error)) return false;
        unsigned long long sequence = 1;
        while (!stopping) {
            received = recv(local, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
            if (received <= 0) break;
            int upload = transport->submitUpload(basePath + "/" + std::to_string(sequence++),
                std::span<const unsigned char>(buffer.data(), static_cast<size_t>(received)), error);
            if (upload < 0 || !transport->waitHeaders(upload, error)) return false;
        }
        std::wstring transportError = transport->failure();
        std::wstring streamError = transport->downloadFailure(download);
        if (error.empty() && !streamError.empty()) error = std::move(streamError);
        if (error.empty() && !transportError.empty()) error = std::move(transportError);
        return error.empty();
    }

    void receiveUdp(uint32_t sessionId, const std::string& destination, std::vector<unsigned char> payload) {
        UdpReceiveHandler handler;
        {
            std::lock_guard lock(udpMutex);
            if (stopping || !udpSessions.contains(sessionId)) return;
            handler = udpHandler;
        }
        ++udpReceived;
        if (handler) handler(sessionId, destination, std::move(payload));
    }

    bool sendUdp(uint32_t sessionId, const std::string& destination,
        const unsigned char* data, size_t length, std::wstring& error) {
        if (stopping) { error = L"VLESS уже остановлен"; return false; }
        // Xray's regular buffer is 8192 bytes and its VLESS UDP writer drops
        // packets that do not fit together with the two-byte length prefix.
        if (!length || length + 2 > 8192) { error = L"VLESS UDP-пакет должен иметь размер 1-8190 байт"; return false; }
        std::vector<std::pair<std::shared_ptr<H2Connection>, int>> expired;
        const unsigned long long now = GetTickCount64();
        {
            std::lock_guard lock(udpMutex);
            for (auto it = udpSessions.begin(); it != udpSessions.end();) {
                if (it->first != sessionId && now - it->second->lastActivityTick.load() > 120000) {
                    if (it->second->transport && it->second->download >= 0)
                        expired.emplace_back(it->second->transport, it->second->download);
                    it = udpSessions.erase(it);
                } else ++it;
            }
        }
        // Never call into H2 while holding udpMutex: downlink callbacks take
        // udpMutex while the H2 session lock is held.
        for (auto& [transport, stream] : expired) transport->cancel(stream);
        std::shared_ptr<UdpSession> udp;
        {
            std::lock_guard lock(udpMutex);
            auto& slot = udpSessions[sessionId];
            if (!slot) {
                slot = std::make_shared<UdpSession>();
                slot->destination = destination;
            }
            udp = slot;
        }
        std::lock_guard sessionLock(udp->mutex);
        if (udp->destination != destination) {
            error = L"VLESS UDP session изменил адрес назначения";
            return false;
        }
        if (!udp->transport || !udp->transport->failure().empty() || udp->download < 0 ||
            !udp->transport->streamOpen(udp->download)) {
            udp->transport = acquireConnection(error);
            if (!udp->transport) return false;
            udp->basePath = utf8(config.path) + utf8(newSessionId());
            udp->sequence = 0;
            udp->download = udp->transport->submitUdpDownload(udp->basePath,
                [this, sessionId, destination](std::vector<unsigned char> packet) {
                    receiveUdp(sessionId, destination, std::move(packet));
                }, error);
            if (udp->download < 0) return false;
        }
        std::vector<unsigned char> framed;
        framed.reserve(length + 2);
        framed.push_back(static_cast<unsigned char>(length >> 8U));
        framed.push_back(static_cast<unsigned char>(length));
        framed.insert(framed.end(), data, data + length);
        std::vector<unsigned char> firstBody;
        std::span<const unsigned char> body = framed;
        if (udp->sequence == 0) {
            std::vector<unsigned char> request;
            if (!makeVlessHeader(config, destination, request, error, 2)) return false;
            firstBody = makeFirstUpload(request, framed);
            body = firstBody;
        }
        int upload = udp->transport->submitUploadAsync(
            udp->basePath + "/" + std::to_string(udp->sequence++), body, error);
        if (upload < 0) return false;
        udp->lastActivityTick = now;
        ++udpSent;
        return true;
    }
};

VlessClient::VlessClient(std::unique_ptr<Impl> implementation) : implementation_(std::move(implementation)) {}
VlessClient::~VlessClient() = default;

namespace {
bool makeLoopbackSocketPair(SOCKET& application, SOCKET& relay, std::wstring& error) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { error = L"VLESS проверка: не удалось создать локальный сокет"; return false; }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR) {
        error = L"VLESS проверка: не удалось открыть локальный канал";
        closesocket(listener);
        return false;
    }
    int addressLength = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressLength) == SOCKET_ERROR) {
        error = L"VLESS проверка: не удалось определить локальный порт";
        closesocket(listener);
        return false;
    }
    application = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (application == INVALID_SOCKET ||
        ::connect(application, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        error = L"VLESS проверка: не удалось соединить локальный канал";
        if (application != INVALID_SOCKET) closesocket(application);
        application = INVALID_SOCKET;
        closesocket(listener);
        return false;
    }
    relay = accept(listener, nullptr, nullptr);
    closesocket(listener);
    if (relay == INVALID_SOCKET) {
        error = L"VLESS проверка: локальный канал не принят";
        closesocket(application);
        application = INVALID_SOCKET;
        return false;
    }
    return true;
}

bool activeTcpProbe(VlessClient& client, std::wstring& error) {
    SOCKET application = INVALID_SOCKET, relay = INVALID_SOCKET;
    if (!makeLoopbackSocketPair(application, relay, error)) return false;
    DWORD timeout = 8000;
    setsockopt(application, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::wstring relayError;
    std::thread worker([&] {
        client.relayTcp("one.one.one.one:80", reinterpret_cast<std::uintptr_t>(relay), relayError, false);
        shutdown(relay, SD_BOTH);
        closesocket(relay);
    });
    static constexpr char request[] =
        "HEAD / HTTP/1.1\r\nHost: one.one.one.one\r\nConnection: close\r\n\r\n";
    bool sent = sendAll(application, reinterpret_cast<const unsigned char*>(request), sizeof(request) - 1);
    std::array<char, 32> response{};
    int received = sent ? recv(application, response.data(), static_cast<int>(response.size()), 0) : SOCKET_ERROR;
    shutdown(application, SD_BOTH);
    closesocket(application);
    worker.join();
    if (received >= 5 && std::string_view(response.data(), static_cast<size_t>(received)).starts_with("HTTP/"))
        return true;
    error = relayError.empty() ? L"VLESS сервер не передал контрольный HTTP-ответ за 8 секунд" : relayError;
    return false;
}
}

std::unique_ptr<VlessClient> VlessClient::connect(const std::wstring& uri, TunnelConnectResult& result) {
    auto implementation = std::make_unique<Impl>();
    if (!parseProfile(uri, implementation->config, result.message)) return nullptr;
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        result.message = L"VLESS: не удалось запустить WinSock";
        return nullptr;
    }
    implementation->winsockStarted = true;
    auto client = std::unique_ptr<VlessClient>(new VlessClient(std::move(implementation)));
    std::wstring probeError;
    if (!activeTcpProbe(*client, probeError)) {
        client->stop();
        result.message = L"VLESS сервер не прошёл проверку: " + probeError;
        return nullptr;
    }
    result.connected = true;
    result.udpEnabled = true;
    result.message = L"VLESS XHTTP/TLS проверен — TCP и UDP через CDN";
    return client;
}

void VlessClient::stop() { if (implementation_) implementation_->stop(); }
bool VlessClient::relayTcp(const std::string& destination, std::uintptr_t socket,
    std::wstring& error, bool socksReply) {
    bool ok = implementation_ && implementation_->relayTcp(destination, static_cast<SOCKET>(socket), error, socksReply);
    bool localClose = error.rfind(L"Локальное приложение закрыло", 0) == 0;
    bool emptyDownlink = error == L"VLESS XHTTP: сервер закрыл stream-down без ответа";
    if (!ok && implementation_ && !localClose && (socksReply || !emptyDownlink))
        implementation_->reportError(error);
    return ok;
}
bool VlessClient::sendUdp(uint32_t sessionId, const std::string& destination,
    const unsigned char* data, size_t length, std::wstring& error) {
    bool ok = implementation_ && implementation_->sendUdp(sessionId, destination, data, length, error);
    if (!ok && implementation_) implementation_->reportError(error);
    return ok;
}
void VlessClient::installUdpReceiveHandler(UdpReceiveHandler handler) {
    if (!implementation_) return;
    std::lock_guard lock(implementation_->udpMutex);
    implementation_->udpHandler = std::move(handler);
}
void VlessClient::setErrorHandler(ErrorHandler handler) {
    if (!implementation_) return;
    std::lock_guard lock(implementation_->udpMutex);
    implementation_->errorHandler = std::move(handler);
}
std::wstring VlessClient::udpDiagnostics() const {
    if (!implementation_) return L"VLESS UDP не запущен";
    std::lock_guard lock(implementation_->udpMutex);
    return L"VLESS UDP: сессий " + std::to_wstring(implementation_->udpSessions.size()) +
        L" • отправлено " + std::to_wstring(implementation_->udpSent.load()) +
        L" • получено " + std::to_wstring(implementation_->udpReceived.load());
}

#ifdef BIG_HEAD_VPN_TESTING
bool vlessProtocolFixtureForTest(std::wstring& error) {
    VlessConfig config;
    if (!parseProfile(L"vless://00112233-4455-6677-8899-aabbccddeeff@cdn.example:443?security=tls&type=xhttp&host=cdn.example&path=%2Fxhttp&mode=packet-up",
            config, error)) return false;
    std::string padding = paddingValue(config, utf8(config.path));
    constexpr std::string_view prefix = "https://cdn.example/xhttp/?_dc=";
    if (!padding.starts_with(prefix)) { error = L"XHTTP fixture: padding должен использовать базовый URL"; return false; }
    size_t encodedLength = hpackHuffmanBytes(std::string_view(padding).substr(prefix.size()));
    if (encodedLength < 98 || encodedLength > 1002) {
        error = L"XHTTP fixture: tokenish padding вышел за диапазон 100-1000";
        return false;
    }
    auto headers = browserFetchHeaders();
    auto has = [&](std::string_view name, std::string_view value = {}) {
        return std::any_of(headers.begin(), headers.end(), [&](const auto& item) {
            return item.first == name && (value.empty() || item.second == value);
        });
    };
    if (!has("sec-ch-ua", "\"Google Chrome\";v=\"149\", \"Chromium\";v=\"149\", \"Not)A;Brand\";v=\"24\"") ||
        !has("sec-fetch-mode", "cors") || !has("priority", "u=1, i") ||
        !has("accept-encoding", "gzip") || has("content-type")) {
        error = L"XHTTP fixture: browser fetch headers не соответствуют Xray";
        return false;
    }
    std::vector<unsigned char> request;
    if (!makeVlessHeader(config, "example.com:443", request, error)) return false;
    const std::vector<unsigned char> expected{
        0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x01, 0x01, 0xbb, 0x02,
        0x0b, 'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm'};
    if (request != expected) { error = L"VLESS fixture: бинарный request header отличается от Xray encoding"; return false; }
    const std::array<unsigned char, 4> clientHelloPrefix{0x16, 0x03, 0x01, 0x02};
    std::vector<unsigned char> firstUpload = makeFirstUpload(request, clientHelloPrefix);
    if (firstUpload.size() != request.size() + clientHelloPrefix.size() ||
        !std::equal(request.begin(), request.end(), firstUpload.begin()) ||
        !std::equal(clientHelloPrefix.begin(), clientHelloPrefix.end(), firstUpload.begin() + request.size())) {
        error = L"XHTTP fixture: пакет /0 должен содержать VLESS header и первый payload";
        return false;
    }
    std::vector<unsigned char> udpRequest;
    if (!makeVlessHeader(config, "stun.l.google.com:19302", udpRequest, error, 2)) return false;
    if (udpRequest.size() < 22 || udpRequest[18] != 2 || udpRequest[19] != 0x4b ||
        udpRequest[20] != 0x66 || udpRequest[21] != 2) {
        error = L"VLESS fixture: UDP request header не соответствует Xray encoding";
        return false;
    }
    const std::array<unsigned char, 3> datagram{0x01, 0x02, 0x03};
    std::vector<unsigned char> framed{0, static_cast<unsigned char>(datagram.size())};
    framed.insert(framed.end(), datagram.begin(), datagram.end());
    std::vector<unsigned char> udpFirstUpload = makeFirstUpload(udpRequest, framed);
    if (udpFirstUpload.size() != udpRequest.size() + 2 + datagram.size() ||
        udpFirstUpload[udpRequest.size()] != 0 ||
        udpFirstUpload[udpRequest.size() + 1] != datagram.size()) {
        error = L"VLESS fixture: UDP datagram framing отличается от Xray";
        return false;
    }
    return true;
}
#endif
