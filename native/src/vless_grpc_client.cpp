#include "vless_grpc_client.h"

#include "reality_tls.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
struct GrpcConfig {
    std::array<unsigned char, 16> id{};
    RealityTlsConfig reality;
    std::string authority;
    std::string path;
};

int hexValue(wchar_t value) {
    if (value >= L'0' && value <= L'9') return value - L'0';
    if (value >= L'a' && value <= L'f') return value - L'a' + 10;
    if (value >= L'A' && value <= L'F') return value - L'A' + 10;
    return -1;
}

bool parseUuid(std::wstring_view value, std::array<unsigned char, 16>& output) {
    size_t nibble = 0;
    output.fill(0);
    for (wchar_t character : value) {
        if (character == L'-') continue;
        int digit = hexValue(character);
        if (digit < 0 || nibble >= 32) return false;
        output[nibble / 2] |= static_cast<unsigned char>(digit << (nibble % 2 == 0 ? 4 : 0));
        ++nibble;
    }
    return nibble == 32;
}

std::wstring decodeUrl(std::wstring_view value) {
    std::wstring result;
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == L'%' && index + 2 < value.size()) {
            int high = hexValue(value[index + 1]), low = hexValue(value[index + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<wchar_t>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        result.push_back(value[index] == L'+' ? L' ' : value[index]);
    }
    return result;
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

std::unordered_map<std::wstring, std::wstring> queryParameters(const std::wstring& uri) {
    std::unordered_map<std::wstring, std::wstring> result;
    size_t query = uri.find(L'?'), fragment = uri.find(L'#', query);
    if (query == std::wstring::npos) return result;
    size_t begin = query + 1;
    const size_t limit = fragment == std::wstring::npos ? uri.size() : fragment;
    while (begin < limit) {
        size_t end = std::min(uri.find(L'&', begin), limit);
        size_t equals = uri.find(L'=', begin);
        if (equals == std::wstring::npos || equals > end) equals = end;
        std::wstring key = decodeUrl(std::wstring_view(uri).substr(begin, equals - begin));
        std::transform(key.begin(), key.end(), key.begin(), towlower);
        result[std::move(key)] = equals == end ? std::wstring{} :
            decodeUrl(std::wstring_view(uri).substr(equals + 1, end - equals - 1));
        begin = end + 1;
    }
    return result;
}

bool parseConfig(const std::wstring& uri, GrpcConfig& config, std::wstring& error) {
    constexpr std::wstring_view prefix = L"vless://";
    size_t at = uri.find(L'@', prefix.size());
    if (uri.rfind(prefix, 0) != 0 || at == std::wstring::npos ||
        !parseUuid(std::wstring_view(uri).substr(prefix.size(), at - prefix.size()), config.id)) {
        error = L"VLESS gRPC: некорректный UUID";
        return false;
    }
    auto parameters = queryParameters(uri);
    std::wstring type = parameters[L"type"], security = parameters[L"security"];
    std::transform(type.begin(), type.end(), type.begin(), towlower);
    std::transform(security.begin(), security.end(), security.begin(), towlower);
    if (type != L"grpc" || security != L"reality") {
        error = L"Этот клиент ожидает VLESS gRPC/REALITY";
        return false;
    }
    if (!parseRealityTlsConfig(uri, config.reality, error)) return false;
    // Chrome's REALITY fingerprint advertises both protocols even though the
    // gRPC layer later uses HTTP/2 with prior knowledge.
    config.reality.alpn = {"h2", "http/1.1"};
    // Xray uses insecure gRPC credentials over its already-established
    // REALITY socket. With no explicit `authority=` in the link it therefore
    // sends an empty override, not the TLS SNI.
    config.authority = utf8(parameters[L"authority"]);
    if (config.authority.empty()) {
        config.authority = utf8(config.reality.endpoint) + ":" +
            std::to_string(config.reality.port);
    }
    std::string service = utf8(parameters[L"servicename"]);
    while (!service.empty() && service.front() == '/') service.erase(service.begin());
    while (!service.empty() && service.back() == '/') service.pop_back();
    config.path = "/" + service + "/Tun";
    return true;
}

bool splitDestination(const std::string& destination, std::string& host, unsigned short& port) {
    size_t colon = destination.rfind(':');
    if (colon == std::string::npos) return false;
    host = destination.substr(0, colon);
    if (host.size() > 1 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    char* end{};
    unsigned long value = strtoul(destination.c_str() + colon + 1, &end, 10);
    if (!end || *end || value == 0 || value > 65535 || host.empty()) return false;
    port = static_cast<unsigned short>(value);
    return true;
}

bool makeVlessRequest(const GrpcConfig& config, const std::string& destination,
    std::vector<unsigned char>& output, std::wstring& error) {
    std::string host;
    unsigned short port{};
    if (!splitDestination(destination, host, port)) {
        error = L"VLESS gRPC получил некорректный адрес назначения";
        return false;
    }
    output = {0};
    output.insert(output.end(), config.id.begin(), config.id.end());
    output.push_back(0);
    output.push_back(1);
    output.push_back(static_cast<unsigned char>(port >> 8));
    output.push_back(static_cast<unsigned char>(port));
    in_addr ipv4{};
    in6_addr ipv6{};
    if (inet_pton(AF_INET, host.c_str(), &ipv4) == 1) {
        output.push_back(1);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&ipv4);
        output.insert(output.end(), bytes, bytes + sizeof(ipv4));
    } else if (inet_pton(AF_INET6, host.c_str(), &ipv6) == 1) {
        output.push_back(3);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&ipv6);
        output.insert(output.end(), bytes, bytes + sizeof(ipv6));
    } else {
        if (host.size() > 255) { error = L"VLESS gRPC: слишком длинное имя сервера"; return false; }
        output.push_back(2);
        output.push_back(static_cast<unsigned char>(host.size()));
        output.insert(output.end(), host.begin(), host.end());
    }
    return true;
}

void appendVarint(std::vector<unsigned char>& output, size_t value) {
    do {
        unsigned char byte = static_cast<unsigned char>(value & 0x7fU);
        value >>= 7U;
        output.push_back(static_cast<unsigned char>(byte | (value ? 0x80U : 0U)));
    } while (value);
}

std::vector<unsigned char> grpcHunk(std::span<const unsigned char> payload) {
    std::vector<unsigned char> protobuf;
    protobuf.reserve(payload.size() + 6);
    protobuf.push_back(0x0a);
    appendVarint(protobuf, payload.size());
    protobuf.insert(protobuf.end(), payload.begin(), payload.end());
    std::vector<unsigned char> framed;
    framed.reserve(protobuf.size() + 5);
    framed.push_back(0);
    uint32_t size = static_cast<uint32_t>(protobuf.size());
    framed.push_back(static_cast<unsigned char>(size >> 24));
    framed.push_back(static_cast<unsigned char>(size >> 16));
    framed.push_back(static_cast<unsigned char>(size >> 8));
    framed.push_back(static_cast<unsigned char>(size));
    framed.insert(framed.end(), protobuf.begin(), protobuf.end());
    return framed;
}

bool readVarint(std::span<const unsigned char> input, size_t& offset, size_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 63 && offset < input.size(); shift += 7) {
        unsigned char byte = input[offset++];
        value |= static_cast<size_t>(byte & 0x7fU) << shift;
        if (!(byte & 0x80U)) return true;
    }
    return false;
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

nghttp2_nv h2Header(std::string_view name, std::string_view value) {
    return {reinterpret_cast<uint8_t*>(const_cast<char*>(name.data())),
        reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())), name.size(), value.size(),
        NGHTTP2_NV_FLAG_NONE};
}

class GrpcRelay {
public:
    GrpcRelay(const GrpcConfig& config, SOCKET local) : config_(config), local_(local) {}
    ~GrpcRelay() { close(); }

    bool connect(std::wstring& error) {
        if (!tls_.connect(config_.reality, error)) return false;
        // Xray's REALITY server intentionally leaves NextProtos empty.  Its
        // gRPC transport starts HTTP/2 with prior knowledge after TLS, so an
        // absent ALPN response is expected here (and is what official Xray
        // does for this profile too).
        if (!tls_.negotiatedAlpn().empty() && tls_.negotiatedAlpn() != "h2") {
            error = L"VLESS gRPC: сервер согласовал неожиданный ALPN";
            return false;
        }
        nghttp2_session_callbacks* callbacks{};
        if (nghttp2_session_callbacks_new(&callbacks) != 0) { error = L"VLESS gRPC: nghttp2 callbacks"; return false; }
        nghttp2_session_callbacks_set_send_callback(callbacks, sendCallback);
        nghttp2_session_callbacks_set_on_header_callback(callbacks, headerCallback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, dataCallback);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, closeCallback);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, frameCallback);
        int result = nghttp2_session_client_new(&session_, callbacks, this);
        nghttp2_session_callbacks_del(callbacks);
        if (result != 0) { error = h2Error(L"создание сеанса", result); return false; }
        // grpc-go/Xray starts with an empty SETTINGS frame, then waits for the
        // server SETTINGS before opening Tun. Some deployed gRPC frontends
        // close clients that optimistically send a request before this step.
        result = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, nullptr, 0);
        if (result != 0 || nghttp2_session_send(session_) != 0) {
            error = h2Error(L"отправка HTTP/2 preface", result);
            return false;
        }
        reader_ = std::thread([this] { readLoop(); });
        {
            std::unique_lock lock(mutex_);
            if (!changed_.wait_for(lock, std::chrono::seconds(5),
                    [this] { return serverSettings_ || failed_; })) {
                error = L"VLESS gRPC: сервер не прислал HTTP/2 SETTINGS";
                return false;
            }
            if (failed_) { error = failure_; return false; }
        }
        std::vector<std::pair<std::string, std::string>> values{
            {":method", "POST"}, {":scheme", "http"}, {":authority", config_.authority},
            {":path", config_.path}, {"content-type", "application/grpc"},
            {"te", "trailers"}, {"grpc-encoding", "identity"},
            {"grpc-accept-encoding", "identity,deflate,gzip"},
            {"user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36"},
        };
        std::vector<nghttp2_nv> headers;
        for (auto& [name, value] : values) headers.push_back(h2Header(name, value));
        nghttp2_data_provider provider{};
        provider.source.ptr = this;
        provider.read_callback = uploadCallback;
        {
            std::lock_guard lock(mutex_);
            streamId_ = nghttp2_submit_request(session_, nullptr, headers.data(), headers.size(), &provider, nullptr);
            if (streamId_ < 0 || nghttp2_session_send(session_) != 0) {
                error = h2Error(L"открытие gRPC Tun", streamId_);
                return false;
            }
        }
#ifdef BIG_HEAD_VPN_TESTING
        fwprintf(stderr, L"grpc_open stream=%d path=%hs authority=%hs\n", streamId_,
            config_.path.c_str(), config_.authority.c_str());
        fflush(stderr);
#endif
        return true;
    }

    bool sendPayload(std::span<const unsigned char> payload, std::wstring& error) {
        auto framed = grpcHunk(payload);
        std::lock_guard lock(mutex_);
        if (failed_ || closed_) { error = failure_.empty() ? L"VLESS gRPC закрыт" : failure_; return false; }
        outgoing_.insert(outgoing_.end(), framed.begin(), framed.end());
        nghttp2_session_resume_data(session_, streamId_);
        int result = nghttp2_session_send(session_);
        if (result != 0) { error = h2Error(L"отправка данных", result); return false; }
        return true;
    }

    std::wstring failure() const {
        std::lock_guard lock(mutex_);
        return failure_;
    }

    void close() {
        if (stopping_.exchange(true)) return;
        tls_.shutdownTransport();
        if (reader_.joinable()) reader_.join();
        if (session_) { nghttp2_session_del(session_); session_ = nullptr; }
        tls_.close();
    }

private:
    static std::wstring h2Error(const wchar_t* action, int code) {
        const char* description = nghttp2_strerror(code);
        return L"VLESS gRPC: " + std::wstring(action) + L": " +
            std::wstring(description, description + strlen(description));
    }

    void fail(std::wstring error) {
        std::lock_guard lock(mutex_);
        if (closed_ || stopping_) return;
        if (!failed_) { failed_ = true; failure_ = std::move(error); }
        shutdown(local_, SD_BOTH);
        changed_.notify_all();
    }

    void readLoop() {
        while (!stopping_) {
            std::vector<unsigned char> input;
            std::wstring error;
            if (!tls_.read(input, error)) {
                bool cleanClose = false;
                {
                    std::lock_guard lock(mutex_);
                    cleanClose = responseHeader_ && incoming_.empty();
                    if (cleanClose) {
                        closed_ = true;
                        shutdown(local_, SD_BOTH);
                        changed_.notify_all();
                    }
                }
                if (cleanClose || stopping_) return;
#ifdef BIG_HEAD_VPN_TESTING
                fwprintf(stderr, L"grpc_tls_read_failed %ls\n", error.c_str());
                fflush(stderr);
#endif
                fail(std::move(error));
                return;
            }
            std::lock_guard lock(mutex_);
            ssize_t received = nghttp2_session_mem_recv(session_, input.data(), input.size());
            if (received < 0) {
                if (!failed_) { failed_ = true; failure_ = h2Error(L"разбор ответа", static_cast<int>(received)); }
                shutdown(local_, SD_BOTH);
                return;
            }
            int sent = nghttp2_session_send(session_);
            if (sent != 0) {
                if (!failed_) { failed_ = true; failure_ = h2Error(L"служебный кадр", sent); }
                shutdown(local_, SD_BOTH);
                return;
            }
        }
    }

    bool consumeMessage(std::span<const unsigned char> message) {
        size_t offset = 0;
        if (message.empty() || message[offset++] != 0x0a) return false;
        size_t payloadLength{};
        if (!readVarint(message, offset, payloadLength) || payloadLength > message.size() - offset) return false;
        std::span<const unsigned char> payload = message.subspan(offset, payloadLength);
        if (!responseHeader_) {
            if (payload.size() < 2 || payload[0] != 0 || payload.size() < 2U + payload[1]) return false;
            payload = payload.subspan(2U + payload[1]);
            responseHeader_ = true;
        }
        return payload.empty() || sendAll(local_, payload.data(), payload.size());
    }

    static ssize_t sendCallback(nghttp2_session*, const uint8_t* data, size_t length,
        int, void* userData) {
        auto& self = *static_cast<GrpcRelay*>(userData);
        std::wstring error;
        if (!self.tls_.write(std::span<const unsigned char>(data, length), error)) {
            self.failed_ = true;
            self.failure_ = std::move(error);
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        return static_cast<ssize_t>(length);
    }

    static ssize_t uploadCallback(nghttp2_session*, int32_t, uint8_t* buffer, size_t length,
        uint32_t* flags, nghttp2_data_source*, void* userData) {
        auto& self = *static_cast<GrpcRelay*>(userData);
        size_t available = self.outgoing_.size() - self.outgoingOffset_;
        if (!available) {
            if (self.uploadEof_) { *flags |= NGHTTP2_DATA_FLAG_EOF; return 0; }
            return NGHTTP2_ERR_DEFERRED;
        }
        size_t copied = std::min(length, available);
        std::copy_n(self.outgoing_.data() + self.outgoingOffset_, copied, buffer);
        self.outgoingOffset_ += copied;
        if (self.outgoingOffset_ == self.outgoing_.size()) {
            self.outgoing_.clear();
            self.outgoingOffset_ = 0;
        }
        return static_cast<ssize_t>(copied);
    }

    static int headerCallback(nghttp2_session*, const nghttp2_frame* frame,
        const uint8_t* name, size_t nameLength, const uint8_t* value, size_t valueLength,
        uint8_t, void* userData) {
        if (frame->hd.type != NGHTTP2_HEADERS || frame->hd.stream_id != static_cast<GrpcRelay*>(userData)->streamId_)
            return 0;
        auto& self = *static_cast<GrpcRelay*>(userData);
        if (nameLength == 7 && memcmp(name, ":status", 7) == 0) {
            self.httpStatus_ = atoi(std::string(reinterpret_cast<const char*>(value), valueLength).c_str());
#ifdef BIG_HEAD_VPN_TESTING
            fwprintf(stderr, L"grpc_status=%d\n", self.httpStatus_);
            fflush(stderr);
#endif
            self.changed_.notify_all();
        } else if (nameLength == 11 && memcmp(name, "grpc-status", 11) == 0) {
            self.grpcStatus_ = atoi(std::string(
                reinterpret_cast<const char*>(value), valueLength).c_str());
        }
        return 0;
    }

    static int dataCallback(nghttp2_session*, uint8_t, int32_t streamId,
        const uint8_t* data, size_t length, void* userData) {
        auto& self = *static_cast<GrpcRelay*>(userData);
        if (streamId != self.streamId_) return 0;
#ifdef BIG_HEAD_VPN_TESTING
        fwprintf(stderr, L"grpc_data bytes=%zu\n", length);
        fflush(stderr);
#endif
        self.incoming_.insert(self.incoming_.end(), data, data + length);
        while (self.incoming_.size() >= 5) {
            if (self.incoming_[0] != 0) return NGHTTP2_ERR_CALLBACK_FAILURE;
            size_t messageLength = (static_cast<size_t>(self.incoming_[1]) << 24U) |
                (static_cast<size_t>(self.incoming_[2]) << 16U) |
                (static_cast<size_t>(self.incoming_[3]) << 8U) | self.incoming_[4];
            if (messageLength > 4U * 1024U * 1024U) return NGHTTP2_ERR_CALLBACK_FAILURE;
            if (self.incoming_.size() < messageLength + 5) break;
            if (!self.consumeMessage(std::span<const unsigned char>(self.incoming_).subspan(5, messageLength)))
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            self.incoming_.erase(self.incoming_.begin(),
                self.incoming_.begin() + static_cast<std::ptrdiff_t>(messageLength + 5));
        }
        return 0;
    }

    static int closeCallback(nghttp2_session*, int32_t streamId, uint32_t errorCode, void* userData) {
        auto& self = *static_cast<GrpcRelay*>(userData);
        if (streamId != self.streamId_) return 0;
#ifdef BIG_HEAD_VPN_TESTING
        fwprintf(stderr, L"grpc_close error=%u status=%d\n", errorCode, self.httpStatus_);
        fflush(stderr);
#endif
        self.closed_ = true;
        const bool clean = errorCode == NGHTTP2_NO_ERROR && self.httpStatus_ == 200 &&
            (self.grpcStatus_ == -1 || self.grpcStatus_ == 0);
        if (!clean && !self.stopping_ && !self.failed_) {
            self.failed_ = true;
            if (self.httpStatus_ != 200)
                self.failure_ = L"VLESS gRPC вернул HTTP " + std::to_wstring(self.httpStatus_);
            else if (self.grpcStatus_ > 0)
                self.failure_ = L"VLESS gRPC завершился со статусом " +
                    std::to_wstring(self.grpcStatus_);
            else
                self.failure_ = L"VLESS gRPC закрыл Tun, код " +
                    std::to_wstring(errorCode);
        }
        shutdown(self.local_, SD_BOTH);
        self.changed_.notify_all();
        return 0;
    }

    static int frameCallback(nghttp2_session*, const nghttp2_frame* frame, void* userData) {
        auto& self = *static_cast<GrpcRelay*>(userData);
        if (frame->hd.type == NGHTTP2_SETTINGS && !(frame->hd.flags & NGHTTP2_FLAG_ACK)) {
            self.serverSettings_ = true;
            self.changed_.notify_all();
#ifdef BIG_HEAD_VPN_TESTING
            fwprintf(stderr, L"grpc_server_settings\n");
            fflush(stderr);
#endif
        }
        return 0;
    }

    const GrpcConfig& config_;
    SOCKET local_{INVALID_SOCKET};
    RealityTls tls_;
    nghttp2_session* session_{};
    int streamId_{-1};
    std::thread reader_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<unsigned char> outgoing_;
    size_t outgoingOffset_{};
    std::vector<unsigned char> incoming_;
    std::atomic_bool stopping_{};
    bool uploadEof_{};
    bool responseHeader_{};
    bool failed_{};
    bool closed_{};
    bool serverSettings_{};
    int httpStatus_{};
    int grpcStatus_{-1};
    std::wstring failure_;
};

bool makeLoopbackSocketPair(SOCKET& application, SOCKET& relay, std::wstring& error) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { error = L"VLESS gRPC: локальный тестовый сокет не создан"; return false; }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener); error = L"VLESS gRPC: локальный тестовый канал не открыт"; return false;
    }
    int addressLength = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressLength);
    application = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (application == INVALID_SOCKET || ::connect(application,
        reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        if (application != INVALID_SOCKET) closesocket(application);
        closesocket(listener); error = L"VLESS gRPC: тестовый канал не соединён"; return false;
    }
    relay = accept(listener, nullptr, nullptr);
    closesocket(listener);
    return relay != INVALID_SOCKET;
}
}

struct VlessGrpcClient::Impl {
    GrpcConfig config;
    std::atomic_bool stopping{};
    bool winsockStarted{};
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<GrpcRelay>> relays;
    ErrorHandler errorHandler;

    ~Impl() { stop(); if (winsockStarted) WSACleanup(); }
    void stop() {
        if (stopping.exchange(true)) return;
        std::vector<std::shared_ptr<GrpcRelay>> active;
        { std::lock_guard lock(mutex); active = relays; }
        for (const auto& relay : active) relay->close();
        { std::lock_guard lock(mutex); relays.clear(); }
    }

    bool relayTcp(const std::string& destination, SOCKET local, std::wstring& error, bool socksReply) {
        if (stopping) { error = L"VLESS gRPC уже остановлен"; return false; }
        auto relay = std::make_shared<GrpcRelay>(config, local);
        { std::lock_guard lock(mutex); relays.push_back(relay); }
        struct Guard {
            Impl* owner; std::shared_ptr<GrpcRelay> relay;
            ~Guard() { relay->close(); std::lock_guard lock(owner->mutex); std::erase(owner->relays, relay); }
        } guard{this, relay};
        if (!relay->connect(error)) return false;
        if (socksReply) {
            static constexpr unsigned char reply[]{5, 0, 0, 1, 0, 0, 0, 0, 0, 0};
            if (!sendAll(local, reply, sizeof(reply))) { error = L"Локальное приложение закрыло SOCKS5"; return false; }
        }
        std::vector<unsigned char> request;
        if (!makeVlessRequest(config, destination, request, error)) return false;
        std::array<unsigned char, 32768> buffer{};
        int received = recv(local, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (received <= 0) { error = L"Локальное приложение закрыло соединение до отправки данных"; return false; }
        request.insert(request.end(), buffer.begin(), buffer.begin() + received);
        if (!relay->sendPayload(request, error)) return false;
        while (!stopping) {
            received = recv(local, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
            if (received <= 0) break;
            if (!relay->sendPayload(std::span<const unsigned char>(buffer.data(), static_cast<size_t>(received)), error))
                return false;
        }
        if (error.empty()) error = relay->failure();
        return error.empty();
    }
};

VlessGrpcClient::VlessGrpcClient(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
VlessGrpcClient::~VlessGrpcClient() = default;

std::unique_ptr<VlessGrpcClient> VlessGrpcClient::connect(const std::wstring& uri,
    TunnelConnectResult& result) {
    auto implementation = std::make_unique<Impl>();
    if (!parseConfig(uri, implementation->config, result.message)) return nullptr;
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        result.message = L"VLESS gRPC: WinSock не запущен";
        return nullptr;
    }
    implementation->winsockStarted = true;
    auto client = std::unique_ptr<VlessGrpcClient>(new VlessGrpcClient(std::move(implementation)));
    SOCKET application = INVALID_SOCKET, relay = INVALID_SOCKET;
    std::wstring error;
    if (!makeLoopbackSocketPair(application, relay, error)) { result.message = error; return nullptr; }
    DWORD timeout = 10000;
    setsockopt(application, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::wstring relayError;
    std::thread worker([&] {
        client->relayTcp("one.one.one.one:80", reinterpret_cast<std::uintptr_t>(relay), relayError, false);
        shutdown(relay, SD_BOTH); closesocket(relay);
    });
    static constexpr char request[] = "HEAD / HTTP/1.1\r\nHost: one.one.one.one\r\nConnection: close\r\n\r\n";
    sendAll(application, reinterpret_cast<const unsigned char*>(request), sizeof(request) - 1);
    std::array<char, 32> response{};
    int received = recv(application, response.data(), static_cast<int>(response.size()), 0);
    shutdown(application, SD_BOTH); closesocket(application); worker.join();
    if (received < 5 || !std::string_view(response.data(), static_cast<size_t>(received)).starts_with("HTTP/")) {
        client->stop();
        result.message = relayError.empty() ? L"VLESS gRPC не передал контрольный HTTP-ответ" : relayError;
        return nullptr;
    }
    result.connected = true;
    result.udpEnabled = false;
    result.message = L"VLESS gRPC/REALITY проверен — TCP работает";
    return client;
}

void VlessGrpcClient::stop() { if (implementation_) implementation_->stop(); }
bool VlessGrpcClient::relayTcp(const std::string& destination, std::uintptr_t socket,
    std::wstring& error, bool socksReply) {
    bool ok = implementation_ && implementation_->relayTcp(destination, static_cast<SOCKET>(socket), error, socksReply);
    if (!ok && implementation_) {
        ErrorHandler handler;
        { std::lock_guard lock(implementation_->mutex); handler = implementation_->errorHandler; }
        if (handler && error.rfind(L"Локальное приложение", 0) != 0) {
            std::wstring target(destination.begin(), destination.end());
            handler(L"TCP-запрос не установился (" + target + L"): " + error);
        }
    }
    return ok;
}
bool VlessGrpcClient::sendUdp(uint32_t, const std::string&, const unsigned char*, size_t,
    std::wstring& error) {
    error = L"VLESS gRPC: UDP пока не включён";
    return false;
}
void VlessGrpcClient::setUdpReceiveHandler(UdpReceiveHandler) {}
void VlessGrpcClient::setErrorHandler(ErrorHandler handler) {
    if (!implementation_) return;
    std::lock_guard lock(implementation_->mutex);
    implementation_->errorHandler = std::move(handler);
}
std::wstring VlessGrpcClient::udpDiagnostics() const { return L"VLESS gRPC: только TCP"; }
