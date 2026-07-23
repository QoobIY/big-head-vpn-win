#include "vless_vision_client.h"

#include "reality_tls.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
struct VisionConfig {
    std::array<unsigned char, 16> id{};
    RealityTlsConfig reality;
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
        output[nibble / 2] |= static_cast<unsigned char>(digit << (nibble % 2 ? 0 : 4));
        ++nibble;
    }
    return nibble == 32;
}

std::wstring queryValue(const std::wstring& uri, std::wstring_view wanted) {
    size_t query = uri.find(L'?'), fragment = uri.find(L'#', query);
    if (query == std::wstring::npos) return {};
    const size_t limit = fragment == std::wstring::npos ? uri.size() : fragment;
    size_t begin = query + 1;
    while (begin < limit) {
        size_t end = std::min(uri.find(L'&', begin), limit);
        size_t equals = uri.find(L'=', begin);
        if (equals != std::wstring::npos && equals < end) {
            std::wstring key = uri.substr(begin, equals - begin);
            std::transform(key.begin(), key.end(), key.begin(), towlower);
            if (key == wanted) return uri.substr(equals + 1, end - equals - 1);
        }
        begin = end + 1;
    }
    return {};
}

bool parseConfig(const std::wstring& uri, VisionConfig& config, std::wstring& error) {
    constexpr std::wstring_view prefix = L"vless://";
    size_t at = uri.find(L'@', prefix.size());
    if (uri.rfind(prefix, 0) != 0 || at == std::wstring::npos ||
        !parseUuid(std::wstring_view(uri).substr(prefix.size(), at - prefix.size()), config.id)) {
        error = L"VLESS Vision: некорректный UUID";
        return false;
    }
    std::wstring type = queryValue(uri, L"type"), security = queryValue(uri, L"security");
    std::wstring flow = queryValue(uri, L"flow");
    std::transform(type.begin(), type.end(), type.begin(), towlower);
    std::transform(security.begin(), security.end(), security.begin(), towlower);
    std::transform(flow.begin(), flow.end(), flow.begin(), towlower);
    if ((type != L"tcp" && type != L"raw") || security != L"reality" ||
        flow != L"xtls-rprx-vision") {
        error = L"Этот клиент ожидает VLESS TCP/REALITY с XTLS Vision";
        return false;
    }
    if (!parseRealityTlsConfig(uri, config.reality, error)) return false;
    config.reality.alpn = {"h2", "http/1.1"};
    return true;
}

bool splitDestination(const std::string& destination, std::string& host, unsigned short& port) {
    size_t colon = destination.rfind(':');
    if (colon == std::string::npos) return false;
    host = destination.substr(0, colon);
    if (host.size() > 1 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    char* end{};
    unsigned long parsed = strtoul(destination.c_str() + colon + 1, &end, 10);
    if (!end || *end || !parsed || parsed > 65535 || host.empty()) return false;
    port = static_cast<unsigned short>(parsed);
    return true;
}

bool makeHeader(const VisionConfig& config, const std::string& destination,
    std::vector<unsigned char>& output, std::wstring& error) {
    std::string host;
    unsigned short port{};
    if (!splitDestination(destination, host, port)) {
        error = L"VLESS Vision получил некорректный адрес назначения";
        return false;
    }
    static constexpr std::string_view flow = "xtls-rprx-vision";
    output = {0};
    output.insert(output.end(), config.id.begin(), config.id.end());
    // protobuf Addons { string Flow = 1; }
    output.push_back(static_cast<unsigned char>(flow.size() + 2));
    output.push_back(0x0a);
    output.push_back(static_cast<unsigned char>(flow.size()));
    output.insert(output.end(), flow.begin(), flow.end());
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
        if (host.size() > 255) { error = L"VLESS Vision: слишком длинное имя назначения"; return false; }
        output.push_back(2);
        output.push_back(static_cast<unsigned char>(host.size()));
        output.insert(output.end(), host.begin(), host.end());
    }
    return true;
}

std::vector<unsigned char> visionFirstBlock(const VisionConfig& config,
    std::span<const unsigned char> payload) {
    // CommandPaddingEnd is fully compatible with Vision readers and keeps the
    // outer REALITY TLS active. Direct/splice is an optimisation, not a wire
    // requirement. Random padding still hides the VLESS header boundary.
    std::array<unsigned char, 2> random{};
    BCryptGenRandom(nullptr, random.data(), random.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    size_t padding = 64 + ((static_cast<size_t>(random[0]) << 8U | random[1]) % 192U);
    std::vector<unsigned char> output;
    output.reserve(21 + payload.size() + padding);
    output.insert(output.end(), config.id.begin(), config.id.end());
    output.push_back(1);
    output.push_back(static_cast<unsigned char>(payload.size() >> 8));
    output.push_back(static_cast<unsigned char>(payload.size()));
    output.push_back(static_cast<unsigned char>(padding >> 8));
    output.push_back(static_cast<unsigned char>(padding));
    output.insert(output.end(), payload.begin(), payload.end());
    size_t begin = output.size();
    output.resize(begin + padding);
    BCryptGenRandom(nullptr, output.data() + begin, static_cast<ULONG>(padding),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return output;
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

class VisionDecoder {
public:
    explicit VisionDecoder(const std::array<unsigned char, 16>& id) : id_(id) {}

    bool consume(std::span<const unsigned char> bytes, SOCKET local, bool& direct,
        std::wstring& error) {
        pending_.insert(pending_.end(), bytes.begin(), bytes.end());
        if (raw_) {
            if (!sendAll(local, pending_.data(), pending_.size())) { error = L"Локальное приложение закрыло соединение"; return false; }
            pending_.clear();
            return true;
        }
        if (!prefixDone_) {
            if (pending_.size() < id_.size()) return true;
            if (!std::equal(id_.begin(), id_.end(), pending_.begin())) {
                error = L"VLESS Vision: ответ не содержит UUID padding";
                return false;
            }
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(id_.size()));
            prefixDone_ = true;
        }
        while (pending_.size() >= 5) {
            unsigned command = pending_[0];
            size_t content = (static_cast<size_t>(pending_[1]) << 8U) | pending_[2];
            size_t padding = (static_cast<size_t>(pending_[3]) << 8U) | pending_[4];
            if (pending_.size() < 5 + content + padding) break;
            if (content && !sendAll(local, pending_.data() + 5, content)) {
                error = L"Локальное приложение закрыло соединение";
                return false;
            }
            pending_.erase(pending_.begin(), pending_.begin() +
                static_cast<std::ptrdiff_t>(5 + content + padding));
            if (command == 1 || command == 2) {
                raw_ = true;
                direct = command == 2;
                if (!pending_.empty() && !sendAll(local, pending_.data(), pending_.size())) {
                    error = L"Локальное приложение закрыло соединение";
                    return false;
                }
                pending_.clear();
                break;
            }
            if (command != 0) { error = L"VLESS Vision: неизвестная команда padding"; return false; }
        }
        return true;
    }

    bool atBlockBoundary() const { return prefixDone_ && pending_.empty(); }

private:
    std::array<unsigned char, 16> id_;
    std::vector<unsigned char> pending_;
    bool prefixDone_{};
    bool raw_{};
};

bool makeLoopbackSocketPair(SOCKET& application, SOCKET& relay, std::wstring& error) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { error = L"VLESS Vision: тестовый сокет не создан"; return false; }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR) { closesocket(listener); error = L"VLESS Vision: тестовый канал не открыт"; return false; }
    int length = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    application = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (application == INVALID_SOCKET || ::connect(application,
        reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        if (application != INVALID_SOCKET) closesocket(application);
        closesocket(listener); error = L"VLESS Vision: тестовый канал не соединён"; return false;
    }
    relay = accept(listener, nullptr, nullptr);
    closesocket(listener);
    return relay != INVALID_SOCKET;
}
}

struct VlessVisionClient::Impl {
    VisionConfig config;
    std::atomic_bool stopping{};
    bool winsockStarted{};
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<RealityTls>> transports;
    ErrorHandler errorHandler;

    ~Impl() { stop(); if (winsockStarted) WSACleanup(); }
    void stop() {
        if (stopping.exchange(true)) return;
        std::vector<std::shared_ptr<RealityTls>> active;
        { std::lock_guard lock(mutex); active = transports; }
        for (const auto& transport : active) transport->shutdownTransport();
        { std::lock_guard lock(mutex); transports.clear(); }
    }

    bool relayTcp(const std::string& destination, SOCKET local, std::wstring& error, bool socksReply) {
        auto tls = std::make_shared<RealityTls>();
        { std::lock_guard lock(mutex); transports.push_back(tls); }
        struct Guard {
            Impl* owner; std::shared_ptr<RealityTls> tls;
            ~Guard() { tls->close(); std::lock_guard lock(owner->mutex); std::erase(owner->transports, tls); }
        } guard{this, tls};
        if (!tls->connect(config.reality, error)) return false;
        if (socksReply) {
            static constexpr unsigned char reply[]{5, 0, 0, 1, 0, 0, 0, 0, 0, 0};
            if (!sendAll(local, reply, sizeof(reply))) { error = L"Локальное приложение закрыло SOCKS5"; return false; }
        }
        std::vector<unsigned char> header;
        if (!makeHeader(config, destination, header, error)) return false;
        std::atomic_bool readerDone{};
        std::wstring readerError;
        std::thread reader([&] {
            VisionDecoder decoder(config.id);
            bool responseHeader = false, direct = false;
            std::vector<unsigned char> headerPending;
            while (!stopping && !readerDone) {
                std::vector<unsigned char> input;
                std::wstring currentError;
                bool received = direct ? tls->readRaw(input, currentError) : tls->read(input, currentError);
                if (!received) {
                    if (!stopping && !(responseHeader && decoder.atBlockBoundary()))
                        readerError = std::move(currentError);
                    break;
                }
                if (!responseHeader) {
                    headerPending.insert(headerPending.end(), input.begin(), input.end());
                    if (headerPending.size() < 2 || headerPending.size() < 2U + headerPending[1]) continue;
                    if (headerPending[0] != 0) { readerError = L"VLESS Vision: неверная версия ответа"; break; }
                    size_t offset = 2U + headerPending[1];
                    input.assign(headerPending.begin() + static_cast<std::ptrdiff_t>(offset), headerPending.end());
                    headerPending.clear();
                    responseHeader = true;
                    if (input.empty()) continue;
                }
                if (!decoder.consume(input, local, direct, readerError)) break;
            }
            readerDone = true;
            shutdown(local, SD_BOTH);
        });
        std::array<unsigned char, 32768> buffer{};
        int count = recv(local, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        bool okay = count > 0;
        if (!okay) error = L"Локальное приложение закрыло соединение до отправки данных";
        if (okay) {
            auto first = visionFirstBlock(config,
                std::span<const unsigned char>(buffer.data(), static_cast<size_t>(count)));
            header.insert(header.end(), first.begin(), first.end());
            okay = tls->write(header, error);
        }
        while (okay && !stopping && !readerDone) {
            count = recv(local, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
            if (count <= 0) break;
            okay = tls->write(std::span<const unsigned char>(buffer.data(), static_cast<size_t>(count)), error);
        }
        readerDone = true;
        tls->shutdownTransport();
        if (reader.joinable()) reader.join();
        if (error.empty() && !readerError.empty() && readerError != L"REALITY direct: соединение закрыто")
            error = std::move(readerError);
        return okay && error.empty();
    }
};

VlessVisionClient::VlessVisionClient(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
VlessVisionClient::~VlessVisionClient() = default;

std::unique_ptr<VlessVisionClient> VlessVisionClient::connect(const std::wstring& uri,
    TunnelConnectResult& result) {
    auto implementation = std::make_unique<Impl>();
    if (!parseConfig(uri, implementation->config, result.message)) return nullptr;
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) { result.message = L"VLESS Vision: WinSock не запущен"; return nullptr; }
    implementation->winsockStarted = true;
    auto client = std::unique_ptr<VlessVisionClient>(new VlessVisionClient(std::move(implementation)));
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
        result.message = relayError.empty() ? L"VLESS Vision не передал контрольный HTTP-ответ" : relayError;
        return nullptr;
    }
    result.connected = true;
    result.udpEnabled = false;
    result.message = L"VLESS TCP/REALITY Vision проверен — TCP работает";
    return client;
}

void VlessVisionClient::stop() { if (implementation_) implementation_->stop(); }
bool VlessVisionClient::relayTcp(const std::string& destination, std::uintptr_t socket,
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
bool VlessVisionClient::sendUdp(uint32_t, const std::string&, const unsigned char*, size_t,
    std::wstring& error) { error = L"VLESS Vision: UDP пока не включён"; return false; }
void VlessVisionClient::setUdpReceiveHandler(UdpReceiveHandler) {}
void VlessVisionClient::setErrorHandler(ErrorHandler handler) {
    if (!implementation_) return;
    std::lock_guard lock(implementation_->mutex);
    implementation_->errorHandler = std::move(handler);
}
std::wstring VlessVisionClient::udpDiagnostics() const { return L"VLESS Vision: только TCP"; }
