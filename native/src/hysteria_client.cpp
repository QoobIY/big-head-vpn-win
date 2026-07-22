#include "hysteria_client.h"

#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <msquic.h>
#include <lsqpack.h>
#include <lsxpack_header.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {
using namespace std::chrono_literals;

std::wstring transportError(QUIC_STATUS status) {
    if (status == QUIC_STATUS_CONNECTION_IDLE)
        return L"Hysteria2-сервер не ответил за 15 секунд. Выберите другой H2-сервер или проверьте, не блокирует ли сеть QUIC/UDP";
    if (status == QUIC_STATUS_CONNECTION_TIMEOUT)
        return L"Истёк тайм-аут подключения к Hysteria2-серверу";
    if (status == QUIC_STATUS_UNREACHABLE)
        return L"Hysteria2-сервер недоступен из текущей сети";
    if (status == QUIC_STATUS_CONNECTION_REFUSED)
        return L"Hysteria2-сервер отклонил подключение";
    if (status == QUIC_STATUS_HANDSHAKE_FAILURE || status == QUIC_STATUS_TLS_ERROR)
        return L"Не удалось согласовать QUIC/TLS с Hysteria2-сервером";
    if (status == QUIC_STATUS_ALPN_NEG_FAILURE)
        return L"Сервер не поддерживает требуемый HTTP/3 протокол";
    wchar_t code[20]{};
    swprintf(code, std::size(code), L"0x%08lX", static_cast<unsigned long>(status));
    return L"QUIC/TLS завершил соединение (" + std::wstring(code) + L")";
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

bool sendSocketAll(SOCKET socket, const uint8_t* data, size_t length) {
    while (length) {
        int chunk = static_cast<int>(std::min(length, static_cast<size_t>(std::numeric_limits<int>::max())));
        int sent = ::send(socket, reinterpret_cast<const char*>(data), chunk, 0);
        if (sent <= 0) return false;
        data += sent; length -= static_cast<size_t>(sent);
    }
    return true;
}

int hex(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::string percentDecode(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            int high = hex(value[i + 1]), low = hex(value[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        result.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return result;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

struct HysteriaConfig {
    std::string host;
    std::string serverName;
    std::string auth;
    uint16_t port{};
    uint64_t maxRx{};
    bool insecure{};
};

std::optional<HysteriaConfig> parseConfig(const std::wstring& input, std::wstring& error) {
    std::string uri = utf8(input);
    size_t schemeEnd = uri.find("://");
    std::string scheme = schemeEnd == std::string::npos ? "" : lower(uri.substr(0, schemeEnd));
    if (scheme != "hysteria2" && scheme != "hy2") { error = L"Нужен профиль hysteria2:// или hy2://"; return std::nullopt; }
    size_t authorityStart = schemeEnd + 3;
    size_t authorityEnd = uri.find_first_of("/?#", authorityStart);
    std::string authority = uri.substr(authorityStart, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - authorityStart);
    size_t at = authority.rfind('@');
    if (at == std::string::npos) { error = L"В Hysteria2-профиле отсутствует пароль"; return std::nullopt; }
    HysteriaConfig config;
    config.auth = percentDecode(std::string_view(authority).substr(0, at));
    std::string endpoint = authority.substr(at + 1);
    std::string port;
    if (!endpoint.empty() && endpoint[0] == '[') {
        size_t close = endpoint.find(']');
        if (close == std::string::npos || close + 2 > endpoint.size() || endpoint[close + 1] != ':') { error = L"Некорректный IPv6-адрес Hysteria2"; return std::nullopt; }
        config.host = endpoint.substr(1, close - 1); port = endpoint.substr(close + 2);
    } else {
        size_t colon = endpoint.rfind(':');
        if (colon == std::string::npos) { error = L"В Hysteria2-профиле отсутствует порт"; return std::nullopt; }
        config.host = endpoint.substr(0, colon); port = endpoint.substr(colon + 1);
    }
    try {
        unsigned long parsed = std::stoul(port);
        if (parsed == 0 || parsed > 65535) throw std::out_of_range("port");
        config.port = static_cast<uint16_t>(parsed);
    } catch (...) { error = L"Некорректный порт Hysteria2"; return std::nullopt; }
    config.serverName = config.host;
    size_t queryStart = uri.find('?', authorityStart);
    if (queryStart != std::string::npos) {
        size_t queryEnd = uri.find('#', queryStart);
        std::string_view query(uri.data() + queryStart + 1, (queryEnd == std::string::npos ? uri.size() : queryEnd) - queryStart - 1);
        size_t begin = 0;
        while (begin <= query.size()) {
            size_t end = query.find('&', begin);
            auto part = query.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
            size_t equals = part.find('=');
            std::string key = lower(percentDecode(part.substr(0, equals)));
            std::string value = equals == std::string_view::npos ? "" : percentDecode(part.substr(equals + 1));
            if (key == "sni" && !value.empty()) config.serverName = value;
            else if (key == "insecure" || key == "allowinsecure") config.insecure = value == "1" || lower(value) == "true";
            else if (key == "down-mbps" || key == "downmbps") {
                try { config.maxRx = static_cast<uint64_t>(std::stoull(value)) * 125000ULL; } catch (...) { config.maxRx = 0; }
            }
            if (end == std::string_view::npos) break;
            begin = end + 1;
        }
    }
    if (config.host.empty() || config.auth.empty()) { error = L"Hysteria2-профиль заполнен не полностью"; return std::nullopt; }
    return config;
}

void appendQuicVarint(std::vector<uint8_t>& output, uint64_t value) {
    if (value <= 63) output.push_back(static_cast<uint8_t>(value));
    else if (value <= 16383) { output.push_back(static_cast<uint8_t>(0x40 | (value >> 8))); output.push_back(static_cast<uint8_t>(value)); }
    else if (value <= 1073741823ULL) {
        output.push_back(static_cast<uint8_t>(0x80 | (value >> 24))); output.push_back(static_cast<uint8_t>(value >> 16));
        output.push_back(static_cast<uint8_t>(value >> 8)); output.push_back(static_cast<uint8_t>(value));
    } else {
        output.push_back(static_cast<uint8_t>(0xC0 | (value >> 56)));
        for (int shift = 48; shift >= 0; shift -= 8) output.push_back(static_cast<uint8_t>(value >> shift));
    }
}

bool readQuicVarint(const std::vector<uint8_t>& input, size_t& offset, uint64_t& value) {
    if (offset >= input.size()) return false;
    size_t length = size_t{1} << (input[offset] >> 6U);
    if (input.size() - offset < length) return false;
    value = input[offset] & 0x3FU;
    for (size_t i = 1; i < length; ++i) value = (value << 8U) | input[offset + i];
    offset += length;
    return true;
}

std::string randomPadding() {
    std::vector<unsigned char> random(514);
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return std::string(256, 'x');
    static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t length = 256 + ((static_cast<size_t>(random[0]) << 8U | random[1]) % 1792U);
    std::string result(length, 'x');
    for (size_t i = 0; i < length; ++i) result[i] = alphabet[random[2 + (i % (random.size() - 2))] % (sizeof(alphabet) - 1)];
    return result;
}

std::optional<std::vector<uint8_t>> authRequest(const HysteriaConfig& config) {
    struct Header { std::string name; std::string value; bool sensitive; };
    std::vector<Header> headers{
        {":authority", "hysteria", false},
        {":method", "POST", false},
        {":path", "/auth", false},
        {":scheme", "https", false},
        {"hysteria-auth", config.auth, false},
        {"hysteria-cc-rx", std::to_string(config.maxRx), false},
        {"hysteria-padding", randomPadding(), false},
        {"content-length", "0", false},
        {"accept-encoding", "gzip", false},
        {"user-agent", "quic-go HTTP/3", false},
    };

    struct lsqpack_enc encoder{};
    lsqpack_enc_preinit(&encoder, nullptr);
    if (lsqpack_enc_start_header(&encoder, 0, 0) != 0) {
        lsqpack_enc_cleanup(&encoder);
        return std::nullopt;
    }

    std::vector<uint8_t> fields(16 * 1024);
    size_t fieldsLength = 0;
    unsigned char encoderOutput[256]{};
    for (const auto& item : headers) {
        std::string storage = item.name + item.value;
        struct lsxpack_header header{};
        lsxpack_header_set_offset2(&header, storage.data(), 0, item.name.size(), item.name.size(), item.value.size());
        size_t encoderLength = sizeof(encoderOutput);
        size_t headerLength = fields.size() - fieldsLength;
        auto flags = static_cast<lsqpack_enc_flags>(LQEF_NO_DYN | (item.sensitive ? LQEF_NEVER_INDEX : LQEF_NO_INDEX));
        if (lsqpack_enc_encode(&encoder, encoderOutput, &encoderLength, fields.data() + fieldsLength, &headerLength, &header, flags) != LQES_OK || encoderLength != 0) {
            lsqpack_enc_cancel_header(&encoder);
            lsqpack_enc_cleanup(&encoder);
            return std::nullopt;
        }
        fieldsLength += headerLength;
    }
    unsigned char prefix[32]{};
    int prefixLength = lsqpack_enc_end_header(&encoder, prefix, sizeof(prefix), nullptr);
    lsqpack_enc_cleanup(&encoder);
    if (prefixLength < 0) return std::nullopt;
    fields.resize(fieldsLength);
    fields.insert(fields.begin(), prefix, prefix + prefixLength);

    std::vector<uint8_t> frame;
    appendQuicVarint(frame, 1); // HEADERS
    appendQuicVarint(frame, fields.size());
    frame.insert(frame.end(), fields.begin(), fields.end());
    return frame;
}
}

#ifdef BIG_HEAD_VPN_TESTING
std::vector<unsigned char> hysteriaAuthFixtureForTest() {
    HysteriaConfig config;
    config.auth = "fixture-secret";
    config.maxRx = 0;
    auto request = authRequest(config);
    return request ? *request : std::vector<unsigned char>{};
}
#endif

struct HysteriaClient::Impl {
    enum class StreamKind { Control, Encoder, Decoder, Auth, PeerUnknown, PeerControl, PeerEncoder, PeerDecoder, Tcp };
    struct StreamContext {
        Impl* owner{};
        StreamKind kind{};
        HQUIC handle{};
        std::vector<uint8_t> received;
        bool typeRead{};
        SOCKET socket{INVALID_SOCKET};
        std::mutex tcpMutex;
        std::condition_variable tcpChanged;
        bool tcpReady{}, tcpFailed{}, applicationReady{}, peerClosed{};
        std::wstring tcpError;
        std::vector<uint8_t> pendingData;
    };
    struct OwnedSend {
        std::vector<uint8_t> data;
        QUIC_BUFFER buffer{};
        StreamKind kind{};
        OwnedSend(std::vector<uint8_t> value, StreamKind valueKind) : data(std::move(value)), kind(valueKind) { buffer.Length = static_cast<uint32_t>(data.size()); buffer.Buffer = data.data(); }
    };
    struct OwnedDatagram {
        std::vector<uint8_t> data;
        QUIC_BUFFER buffer{};
        explicit OwnedDatagram(std::vector<uint8_t> value) : data(std::move(value)) { buffer.Length = static_cast<uint32_t>(data.size()); buffer.Buffer = data.data(); }
    };
    struct UdpFragments {
        uint8_t count{};
        size_t received{};
        std::string address;
        std::vector<std::vector<uint8_t>> parts;
        std::chrono::steady_clock::time_point updated;
    };

    HMODULE module{};
    const QUIC_API_TABLE* api{};
    MsQuicCloseFn closeApi{};
    HQUIC registration{}, configuration{}, connection{};
    std::vector<std::unique_ptr<StreamContext>> streams;
    StreamContext* decoderStream{};
    StreamContext* authStream{};
    std::mutex mutex;
    std::condition_variable stateChanged;
    bool finished{}, success{}, udpEnabled{}, shutdownComplete{};
    std::wstring message;
    HysteriaConfig config;
    struct lsqpack_dec decoder{};
    struct lsxpack_header decodedHeader{};
    char decodeBuffer[8192]{};
    int responseStatus{};
    bool responseUdp{};
    size_t authBytesReceived{};
    uint64_t peerCloseCode{std::numeric_limits<uint64_t>::max()};
    unsigned completedStarts{};
    unsigned completedSends{};
    unsigned canceledSends{};
    unsigned canceledSendMask{};
    std::atomic_uint16_t maxDatagramLength{1200};
    std::atomic_uint16_t udpPacketId{};
    std::atomic_bool datagramSendEnabled{};
    std::atomic_uint64_t udpQueued{}, udpSent{}, udpAcknowledged{}, udpLost{}, udpCanceled{}, udpRawReceived{};
    std::mutex udpMutex;
    UdpReceiveHandler udpHandler;
    ErrorHandler errorHandler;
    std::unordered_map<uint64_t, UdpFragments> udpFragments;
    std::atomic_bool stopping{};
    std::atomic_bool runtimeFailed{};
    bool cleaned{};

    static struct lsqpack_dec_hset_if decoderCallbacks;

    explicit Impl(HysteriaConfig value) : config(std::move(value)) {
        lsqpack_dec_init(&decoder, nullptr, 0, 0, &decoderCallbacks, static_cast<lsqpack_dec_opts>(0));
    }

    ~Impl() { stop(); cleanup(); lsqpack_dec_cleanup(&decoder); }

    void finish(bool ok, std::wstring text) {
        std::lock_guard lock(mutex);
        if (finished) return;
        finished = true; success = ok; message = std::move(text);
        stateChanged.notify_all();
    }

    void reportError(const std::wstring& error) {
        ErrorHandler handler;
        {
            std::lock_guard lock(udpMutex);
            handler = errorHandler;
        }
        if (handler && !stopping) handler(error);
    }

    void failRuntime(std::wstring error) {
        if (stopping || runtimeFailed.exchange(true)) return;
        {
            std::lock_guard lock(mutex);
            for (auto& stream : streams) {
                if (stream->kind != StreamKind::Tcp) continue;
                std::lock_guard streamLock(stream->tcpMutex);
                stream->tcpFailed = true;
                stream->tcpError = error;
                if (stream->socket != INVALID_SOCKET) ::shutdown(stream->socket, SD_BOTH);
                stream->tcpChanged.notify_all();
            }
        }
        reportError(error);
    }

    bool initialize(std::wstring& error) {
        wchar_t executable[MAX_PATH]{};
        DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        if (!length || length == std::size(executable)) { error = L"Не удалось определить папку приложения"; return false; }
        auto dll = std::filesystem::path(executable).parent_path() / L"msquic.dll";
        module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) { error = L"Рядом с EXE отсутствует msquic.dll"; return false; }
        auto open = reinterpret_cast<MsQuicOpenVersionFn>(GetProcAddress(module, "MsQuicOpenVersion"));
        closeApi = reinterpret_cast<MsQuicCloseFn>(GetProcAddress(module, "MsQuicClose"));
        if (!open || !closeApi || QUIC_FAILED(open(QUIC_API_VERSION_2, reinterpret_cast<const void**>(&api)))) { error = L"Не удалось открыть MsQuic API"; return false; }
        QUIC_REGISTRATION_CONFIG registrationConfig{"BigHeadVPN", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
        if (QUIC_FAILED(api->RegistrationOpen(&registrationConfig, &registration))) { error = L"Не удалось создать QUIC registration"; return false; }
        QUIC_SETTINGS settings{};
        settings.IdleTimeoutMs = 30000; settings.IsSet.IdleTimeoutMs = TRUE;
        settings.HandshakeIdleTimeoutMs = 15000; settings.IsSet.HandshakeIdleTimeoutMs = TRUE;
        settings.KeepAliveIntervalMs = 10000; settings.IsSet.KeepAliveIntervalMs = TRUE;
        settings.DatagramReceiveEnabled = TRUE; settings.IsSet.DatagramReceiveEnabled = TRUE;
        settings.PeerUnidiStreamCount = 3; settings.IsSet.PeerUnidiStreamCount = TRUE;
        settings.StreamRecvWindowDefault = 8 * 1024 * 1024; settings.IsSet.StreamRecvWindowDefault = TRUE;
        settings.ConnFlowControlWindow = 20 * 1024 * 1024; settings.IsSet.ConnFlowControlWindow = TRUE;
        uint8_t alpnBytes[] = {'h', '3'};
        QUIC_BUFFER alpn{2, alpnBytes};
        if (QUIC_FAILED(api->ConfigurationOpen(registration, &alpn, 1, &settings, sizeof(settings), nullptr, &configuration))) { error = L"Не удалось создать QUIC configuration"; return false; }
        QUIC_CREDENTIAL_CONFIG credentials{};
        credentials.Type = QUIC_CREDENTIAL_TYPE_NONE;
        credentials.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
        if (config.insecure) credentials.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
        if (QUIC_FAILED(api->ConfigurationLoadCredential(configuration, &credentials))) { error = L"Не удалось загрузить TLS Schannel"; return false; }
        if (QUIC_FAILED(api->ConnectionOpen(registration, connectionCallback, this, &connection))) { error = L"Не удалось создать QUIC connection"; return false; }
        if (QUIC_FAILED(api->ConnectionStart(connection, configuration, QUIC_ADDRESS_FAMILY_UNSPEC, config.serverName.c_str(), config.port))) { error = L"Не удалось начать QUIC handshake"; return false; }
        return true;
    }

    bool waitForAuth(HysteriaConnectResult& result) {
        std::unique_lock lock(mutex);
        if (!stateChanged.wait_for(lock, 20s, [&] { return finished; })) {
            lock.unlock(); finish(false, L"Тайм-аут подключения Hysteria2"); lock.lock();
        }
        result.connected = success; result.udpEnabled = udpEnabled; result.message = message;
        return success;
    }

    StreamContext* openStream(StreamKind kind, QUIC_STREAM_OPEN_FLAGS flags, std::vector<uint8_t> initial, QUIC_SEND_FLAGS sendFlags) {
        if (stopping || !api || !connection) return nullptr;
        auto context = std::make_unique<StreamContext>();
        context->owner = this; context->kind = kind;
        if (QUIC_FAILED(api->StreamOpen(connection, flags, streamCallback, context.get(), &context->handle))) return nullptr;
        StreamContext* result = context.get();
        {
            std::lock_guard lock(mutex);
            streams.push_back(std::move(context));
        }
        if (!send(result, std::move(initial), sendFlags)) return nullptr;
        return result;
    }

    bool send(StreamContext* stream, std::vector<uint8_t> data, QUIC_SEND_FLAGS flags) {
        auto owned = std::make_unique<OwnedSend>(std::move(data), stream->kind);
        QUIC_STATUS status = api->StreamSend(stream->handle, &owned->buffer, 1, flags, owned.get());
        if (QUIC_FAILED(status)) return false;
        owned.release();
        return true;
    }

    void onConnected() {
        auto request = authRequest(config);
        if (!request) { finish(false, L"Не удалось закодировать HTTP/3 auth-запрос"); return; }
        // Hysteria uses raw QUIC DATAGRAM frames, not RFC 9297 HTTP Datagrams.
        // Advertising SETTINGS_H3_DATAGRAM makes HTTP/3 consume those frames
        // before Hysteria's UDP session manager can see them.
        if (!openStream(StreamKind::Control, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, {0x00, 0x04, 0x02, 0x06, 0x00}, QUIC_SEND_FLAG_START) ||
            !(authStream = openStream(StreamKind::Auth, QUIC_STREAM_OPEN_FLAG_NONE, std::move(*request), QUIC_SEND_FLAG_START))) {
            finish(false, L"Не удалось открыть HTTP/3 streams");
        }
    }

    void processPeerStream(StreamContext& stream, const uint8_t* data, size_t length) {
        stream.received.insert(stream.received.end(), data, data + length);
        if (!stream.typeRead) {
            size_t offset = 0; uint64_t type = 0;
            if (!readQuicVarint(stream.received, offset, type)) return;
            stream.typeRead = true;
            stream.kind = type == 0 ? StreamKind::PeerControl : type == 2 ? StreamKind::PeerEncoder : type == 3 ? StreamKind::PeerDecoder : StreamKind::PeerUnknown;
            stream.received.erase(stream.received.begin(), stream.received.begin() + static_cast<std::ptrdiff_t>(offset));
        }
        if (stream.kind == StreamKind::PeerEncoder && !stream.received.empty()) {
            if (lsqpack_dec_enc_in(&decoder, stream.received.data(), stream.received.size()) != 0) finish(false, L"Ошибка QPACK encoder stream");
            stream.received.clear();
        } else if (stream.kind != StreamKind::PeerControl) stream.received.clear();
    }

    void processAuth(StreamContext& stream, const uint8_t* data, size_t length) {
        authBytesReceived += length;
        stream.received.insert(stream.received.end(), data, data + length);
        size_t offset = 0;
        while (offset < stream.received.size()) {
            size_t frameStart = offset; uint64_t type = 0, frameLength = 0;
            if (!readQuicVarint(stream.received, offset, type) || !readQuicVarint(stream.received, offset, frameLength)) { offset = frameStart; break; }
            if (frameLength > 64 * 1024 || stream.received.size() - offset < frameLength) { offset = frameStart; break; }
            if (type == 1) decodeAuthHeaders(stream.received.data() + offset, static_cast<size_t>(frameLength));
            offset += static_cast<size_t>(frameLength);
        }
        if (offset) stream.received.erase(stream.received.begin(), stream.received.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    void processTcp(StreamContext& stream, const uint8_t* data, size_t length) {
        std::vector<uint8_t> deliver;
        SOCKET socket = INVALID_SOCKET;
        {
            std::lock_guard lock(stream.tcpMutex);
            if (!stream.tcpReady) {
                stream.received.insert(stream.received.end(), data, data + length);
                if (stream.received.empty()) return;
                size_t offset = 1;
                uint64_t messageLength = 0, paddingLength = 0;
                if (!readQuicVarint(stream.received, offset, messageLength) || messageLength > 2048 ||
                    stream.received.size() - offset < messageLength) return;
                size_t messageOffset = offset;
                offset += static_cast<size_t>(messageLength);
                if (!readQuicVarint(stream.received, offset, paddingLength) || paddingLength > 4096 ||
                    stream.received.size() - offset < paddingLength) return;
                offset += static_cast<size_t>(paddingLength);
                if (stream.received[0] != 0) {
                    stream.tcpFailed = true;
                    std::string message(reinterpret_cast<const char*>(stream.received.data() + messageOffset), static_cast<size_t>(messageLength));
                    stream.tcpError = L"Сервер отклонил TCP-подключение";
                    if (!message.empty()) stream.tcpError += L" (см. адрес назначения)";
                } else stream.tcpReady = true;
                if (stream.received.size() > offset)
                    stream.pendingData.assign(stream.received.begin() + static_cast<std::ptrdiff_t>(offset), stream.received.end());
                stream.received.clear();
                stream.tcpChanged.notify_all();
                return;
            }
            if (!stream.applicationReady) {
                stream.pendingData.insert(stream.pendingData.end(), data, data + length);
                return;
            }
            socket = stream.socket;
            deliver.assign(data, data + length);
        }
        if (socket != INVALID_SOCKET && !deliver.empty() && !sendSocketAll(socket, deliver.data(), deliver.size())) {
            std::lock_guard lock(stream.tcpMutex);
            stream.tcpFailed = true;
            stream.tcpError = L"Локальное приложение закрыло TCP-соединение";
            if (stream.socket != INVALID_SOCKET) ::shutdown(stream.socket, SD_BOTH);
            stream.tcpChanged.notify_all();
        }
    }

    void decodeAuthHeaders(const uint8_t* bytes, size_t length) {
        const unsigned char* cursor = bytes;
        unsigned char acknowledgment[32]{}; size_t acknowledgmentLength = sizeof(acknowledgment);
        auto status = lsqpack_dec_header_in(&decoder, this, 0, length, &cursor, length, acknowledgment, &acknowledgmentLength);
        if (status == LQRHS_ERROR || status == LQRHS_NEED) { finish(false, L"Некорректный HTTP/3 ответ Hysteria2"); return; }
        if (acknowledgmentLength && decoderStream) send(decoderStream, std::vector<uint8_t>(acknowledgment, acknowledgment + acknowledgmentLength), QUIC_SEND_FLAG_NONE);
        if (status == LQRHS_DONE) {
            if (responseStatus == 233) {
                udpEnabled = responseUdp;
                finish(true, responseUdp ? L"Hysteria2 авторизована — сервер поддерживает TCP и UDP" : L"Hysteria2 авторизована — сервер отключил UDP");
            } else finish(false, L"Сервер Hysteria2 отклонил авторизацию: HTTP " + std::to_wstring(responseStatus));
        }
    }

    static void decoderUnblocked(void*) {}
    static struct lsxpack_header* decoderPrepare(void* context, struct lsxpack_header* header, size_t space) {
        auto& self = *static_cast<Impl*>(context);
        if (space > sizeof(self.decodeBuffer)) return nullptr;
        if (header) { header->buf = self.decodeBuffer; header->val_len = static_cast<lsxpack_strlen_t>(space); return header; }
        lsxpack_header_prepare_decode(&self.decodedHeader, self.decodeBuffer, 0, space);
        return &self.decodedHeader;
    }
    static int decoderProcess(void* context, struct lsxpack_header* header) {
        auto& self = *static_cast<Impl*>(context);
        std::string name(header->buf + header->name_offset, header->name_len);
        std::string value(header->buf + header->val_offset, header->val_len);
        name = lower(std::move(name));
        if (name == ":status") { try { self.responseStatus = std::stoi(value); } catch (...) { self.responseStatus = 0; } }
        else if (name == "hysteria-udp") self.responseUdp = lower(value) == "true";
        return 0;
    }

    static QUIC_STATUS QUIC_API connectionCallback(HQUIC, void* context, QUIC_CONNECTION_EVENT* event) {
        auto& self = *static_cast<Impl*>(context);
        switch (event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED: self.onConnected(); break;
        case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
            self.datagramSendEnabled = event->DATAGRAM_STATE_CHANGED.SendEnabled != FALSE;
            if (event->DATAGRAM_STATE_CHANGED.SendEnabled && event->DATAGRAM_STATE_CHANGED.MaxSendLength > 64)
                self.maxDatagramLength = event->DATAGRAM_STATE_CHANGED.MaxSendLength;
            self.stateChanged.notify_all();
            break;
        case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
            ++self.udpRawReceived;
            self.receiveUdp(event->DATAGRAM_RECEIVED.Buffer->Buffer, event->DATAGRAM_RECEIVED.Buffer->Length);
            break;
        case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED: {
            auto state = event->DATAGRAM_SEND_STATE_CHANGED.State;
            if (state == QUIC_DATAGRAM_SEND_SENT) ++self.udpSent;
            else if (state == QUIC_DATAGRAM_SEND_ACKNOWLEDGED || state == QUIC_DATAGRAM_SEND_ACKNOWLEDGED_SPURIOUS) ++self.udpAcknowledged;
            else if (state == QUIC_DATAGRAM_SEND_LOST_DISCARDED) ++self.udpLost;
            else if (state == QUIC_DATAGRAM_SEND_CANCELED) ++self.udpCanceled;
            if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(event->DATAGRAM_SEND_STATE_CHANGED.State))
                delete static_cast<OwnedDatagram*>(event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
            break;
        }
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT: {
            std::wstring error = transportError(event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
            self.finish(false, error);
            self.failRuntime(std::move(error));
            break;
        }
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            self.peerCloseCode = event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode;
            if (!self.stopping) {
                std::wstring error = L"Сервер Hysteria2 закрыл соединение, H3 код " + std::to_wstring(self.peerCloseCode);
                self.finish(false, error);
                self.failRuntime(std::move(error));
            }
            break;
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
            bool reportGracefulClose = false;
            {
                std::lock_guard lock(self.mutex);
                self.shutdownComplete = true;
                reportGracefulClose = !self.finished && self.peerCloseCode == 0x100;
                self.stateChanged.notify_all();
            }
            if (reportGracefulClose) {
                self.finish(false, L"Сервер завершил HTTP/3 без auth-ответа (получено " + std::to_wstring(self.authBytesReceived) +
                    L" байт, streams " + std::to_wstring(self.completedStarts) + L"/2, отправки " +
                    std::to_wstring(self.completedSends) + L"/2, отменено " + std::to_wstring(self.canceledSends) +
                    L", mask " + std::to_wstring(self.canceledSendMask) + L")");
            }
            break;
        }
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            if (event->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) {
                auto stream = std::make_unique<StreamContext>(); stream->owner = &self; stream->kind = StreamKind::PeerUnknown; stream->handle = event->PEER_STREAM_STARTED.Stream;
                self.api->SetCallbackHandler(stream->handle, reinterpret_cast<void*>(streamCallback), stream.get());
                std::lock_guard lock(self.mutex); self.streams.push_back(std::move(stream));
            }
            break;
        default: break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    void receiveUdp(const uint8_t* bytes, size_t length) {
        if (length < 9) return;
        uint32_t session = (static_cast<uint32_t>(bytes[0]) << 24U) | (static_cast<uint32_t>(bytes[1]) << 16U) |
            (static_cast<uint32_t>(bytes[2]) << 8U) | bytes[3];
        uint16_t packet = static_cast<uint16_t>((bytes[4] << 8U) | bytes[5]);
        uint8_t fragment = bytes[6], count = bytes[7];
        std::vector<uint8_t> message(bytes, bytes + length); size_t offset = 8; uint64_t addressLength = 0;
        if (!count || fragment >= count || !readQuicVarint(message, offset, addressLength) || addressLength > message.size() - offset) return;
        std::string address(reinterpret_cast<const char*>(message.data() + offset), static_cast<size_t>(addressLength));
        offset += static_cast<size_t>(addressLength);
        std::vector<uint8_t> payload(message.begin() + static_cast<std::ptrdiff_t>(offset), message.end());
        UdpReceiveHandler handler;
        if (count == 1) {
            std::lock_guard lock(udpMutex); handler = udpHandler;
        } else {
            std::lock_guard lock(udpMutex);
            auto now = std::chrono::steady_clock::now();
            if (udpFragments.size() > 256)
                std::erase_if(udpFragments, [&](const auto& item) { return now - item.second.updated > 10s; });
            auto& state = udpFragments[(static_cast<uint64_t>(session) << 16U) | packet];
            if (!state.count) { state.count = count; state.address = address; state.parts.resize(count); }
            if (state.count != count) { udpFragments.erase((static_cast<uint64_t>(session) << 16U) | packet); return; }
            state.updated = now;
            if (state.parts[fragment].empty()) { state.parts[fragment] = std::move(payload); ++state.received; }
            if (state.received != count) return;
            payload.clear(); for (auto& part : state.parts) payload.insert(payload.end(), part.begin(), part.end());
            address = state.address; udpFragments.erase((static_cast<uint64_t>(session) << 16U) | packet); handler = udpHandler;
        }
        if (handler) handler(session, address, std::move(payload));
    }

    bool sendUdp(uint32_t session, const std::string& destination, const uint8_t* data, size_t length, std::wstring& error) {
        if (stopping) { error = L"Hysteria2 остановлена"; return false; }
        if (!udpEnabled) { error = L"Hysteria2-сервер отключил UDP"; return false; }
        if (!datagramSendEnabled.load()) {
            std::unique_lock lock(mutex);
            stateChanged.wait_for(lock, 2s, [&] { return datagramSendEnabled.load() || shutdownComplete; });
        }
        if (!datagramSendEnabled.load()) { error = L"QUIC-сервер не согласовал отправку UDP datagram"; return false; }
        size_t maxLength = maxDatagramLength.load();
        std::vector<uint8_t> prefix;
        prefix.reserve(16 + destination.size());
        prefix.push_back(static_cast<uint8_t>(session >> 24U)); prefix.push_back(static_cast<uint8_t>(session >> 16U));
        prefix.push_back(static_cast<uint8_t>(session >> 8U)); prefix.push_back(static_cast<uint8_t>(session));
        prefix.push_back(0); prefix.push_back(0);
        prefix.push_back(0); prefix.push_back(1); appendQuicVarint(prefix, destination.size());
        prefix.insert(prefix.end(), destination.begin(), destination.end());
        if (maxLength <= prefix.size()) { error = L"QUIC datagram слишком мал"; return false; }
        size_t chunkSize = maxLength - prefix.size();
        size_t count = std::max<size_t>(1, (length + chunkSize - 1) / chunkSize);
        if (count > 255) { error = L"UDP-пакет слишком велик"; return false; }
        // The reference Hysteria client uses packet ID 0 for an unfragmented
        // datagram. A non-zero ID only identifies parts during reassembly.
        uint16_t packet = count == 1 ? 0 : ++udpPacketId;
        prefix[4] = static_cast<uint8_t>(packet >> 8U); prefix[5] = static_cast<uint8_t>(packet);
        for (size_t fragment = 0, position = 0; fragment < count; ++fragment) {
            size_t chunk = std::min(chunkSize, length - position);
            auto frame = prefix; frame[6] = static_cast<uint8_t>(fragment); frame[7] = static_cast<uint8_t>(count);
            if (chunk) frame.insert(frame.end(), data + position, data + position + chunk);
            position += chunk;
            auto owned = std::make_unique<OwnedDatagram>(std::move(frame));
            QUIC_STATUS status = api->DatagramSend(connection, &owned->buffer, 1, QUIC_SEND_FLAG_NONE, owned.get());
            if (QUIC_FAILED(status)) { error = L"Не удалось отправить Hysteria2 UDP datagram"; return false; }
            ++udpQueued;
            owned.release();
        }
        return true;
    }

    std::wstring udpDiagnostics() const {
        return L"enabled=" + std::to_wstring(datagramSendEnabled.load()) +
            L" max=" + std::to_wstring(maxDatagramLength.load()) +
            L" queued=" + std::to_wstring(udpQueued.load()) +
            L" sent=" + std::to_wstring(udpSent.load()) +
            L" ack=" + std::to_wstring(udpAcknowledged.load()) +
            L" lost=" + std::to_wstring(udpLost.load()) +
            L" canceled=" + std::to_wstring(udpCanceled.load()) +
            L" received=" + std::to_wstring(udpRawReceived.load());
    }

    static QUIC_STATUS QUIC_API streamCallback(HQUIC, void* context, QUIC_STREAM_EVENT* event) {
        auto& stream = *static_cast<StreamContext*>(context);
        auto& self = *stream.owner;
        switch (event->Type) {
        case QUIC_STREAM_EVENT_START_COMPLETE:
            if (QUIC_SUCCEEDED(event->START_COMPLETE.Status)) ++self.completedStarts;
            else if (stream.kind == StreamKind::Tcp) {
                std::wstring error = L"Не удалось запустить Hysteria2 TCP stream, код " + std::to_wstring(event->START_COMPLETE.Status);
                {
                    std::lock_guard lock(stream.tcpMutex);
                    stream.tcpFailed = true;
                    stream.tcpError = error;
                    if (stream.socket != INVALID_SOCKET) ::shutdown(stream.socket, SD_BOTH);
                    stream.tcpChanged.notify_all();
                }
                self.reportError(error);
            } else self.finish(false, L"Не удалось запустить HTTP/3 stream, код " + std::to_wstring(event->START_COMPLETE.Status));
            break;
        case QUIC_STREAM_EVENT_RECEIVE:
            for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
                const auto& buffer = event->RECEIVE.Buffers[i];
                if (stream.kind == StreamKind::Auth) self.processAuth(stream, buffer.Buffer, buffer.Length);
                else if (stream.kind == StreamKind::Tcp) self.processTcp(stream, buffer.Buffer, buffer.Length);
                else if (stream.kind == StreamKind::PeerUnknown || stream.kind == StreamKind::PeerControl || stream.kind == StreamKind::PeerEncoder || stream.kind == StreamKind::PeerDecoder)
                    self.processPeerStream(stream, buffer.Buffer, buffer.Length);
            }
            break;
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            ++self.completedSends;
            {
                auto* sendContext = static_cast<OwnedSend*>(event->SEND_COMPLETE.ClientContext);
                if (!event->SEND_COMPLETE.Canceled && sendContext->kind == StreamKind::Auth)
                    self.api->StreamShutdown(stream.handle, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
            }
            if (event->SEND_COMPLETE.Canceled) {
                ++self.canceledSends;
                auto* sendContext = static_cast<OwnedSend*>(event->SEND_COMPLETE.ClientContext);
                self.canceledSendMask |= 1U << static_cast<unsigned>(sendContext->kind);
            }
            delete static_cast<OwnedSend*>(event->SEND_COMPLETE.ClientContext);
            break;
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            if (stream.kind == StreamKind::Auth) self.finish(false, L"HTTP/3 auth stream прерван сервером");
            else if (stream.kind == StreamKind::Tcp) {
                std::lock_guard lock(stream.tcpMutex); stream.tcpFailed = true; stream.tcpError = L"TCP stream прерван сервером";
                if (stream.socket != INVALID_SOCKET) ::shutdown(stream.socket, SD_BOTH);
                stream.tcpChanged.notify_all();
            }
            break;
        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            if (stream.kind == StreamKind::Tcp) {
                std::lock_guard lock(stream.tcpMutex); stream.peerClosed = true;
                if (stream.socket != INVALID_SOCKET) ::shutdown(stream.socket, SD_SEND);
                stream.tcpChanged.notify_all();
            }
            break;
        default: break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    void stop() {
        if (stopping.exchange(true)) return;
        {
            std::lock_guard lock(udpMutex);
            udpHandler = {};
        }
        // Wake every external SOCKS/WinDivert relay before releasing any
        // StreamContext.  The owners are destroyed by the UI immediately
        // after stop() and can then join their worker threads safely.
        {
            std::lock_guard lock(mutex);
            for (auto& stream : streams) {
                if (stream->kind != StreamKind::Tcp) continue;
                std::lock_guard streamLock(stream->tcpMutex);
                stream->tcpFailed = true;
                stream->tcpError = L"Hysteria2 остановлена";
                if (stream->socket != INVALID_SOCKET) ::shutdown(stream->socket, SD_BOTH);
                stream->tcpChanged.notify_all();
            }
        }
        if (connection && api) {
            api->ConnectionShutdown(connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0x100);
            std::unique_lock lock(mutex);
            stateChanged.wait_for(lock, 2s, [&] { return shutdownComplete; });
        }
    }

    void cleanup() {
        if (cleaned) return;
        cleaned = true;
        if (api) {
            for (auto& stream : streams) if (stream->handle) api->StreamClose(stream->handle);
            streams.clear();
            if (connection) api->ConnectionClose(connection);
            if (configuration) api->ConfigurationClose(configuration);
            if (registration) api->RegistrationClose(registration);
        }
        connection = nullptr; configuration = nullptr; registration = nullptr;
        if (api && closeApi) closeApi(api);
        api = nullptr;
        if (module) FreeLibrary(module);
        module = nullptr;
    }

    bool relayTcp(const std::string& destination, SOCKET socket, std::wstring& error, bool socksReply) {
        if (runtimeFailed) { error = L"Соединение Hysteria2 потеряно — переподключитесь"; return false; }
        std::string padding = randomPadding().substr(0, 64 + (GetTickCount64() % 448));
        std::vector<uint8_t> request;
        appendQuicVarint(request, 0x401);
        appendQuicVarint(request, destination.size());
        request.insert(request.end(), destination.begin(), destination.end());
        appendQuicVarint(request, padding.size());
        request.insert(request.end(), padding.begin(), padding.end());
        StreamContext* stream = openStream(StreamKind::Tcp, QUIC_STREAM_OPEN_FLAG_NONE, std::move(request), QUIC_SEND_FLAG_START);
        if (!stream) { error = L"Не удалось открыть Hysteria2 TCP stream"; return false; }
        stream->socket = socket;
        {
            std::unique_lock lock(stream->tcpMutex);
            if (!stream->tcpChanged.wait_for(lock, 15s, [&] { return stream->tcpReady || stream->tcpFailed; })) {
                stream->tcpFailed = true; stream->tcpError = L"Тайм-аут TCP-подключения через Hysteria2";
            }
            if (stream->tcpFailed) { error = stream->tcpError; return false; }
        }
        if (socksReply) {
            const unsigned char reply[]{5, 0, 0, 1, 0, 0, 0, 0, 0, 0};
            if (!sendSocketAll(socket, reply, sizeof(reply))) { error = L"Локальное SOCKS5-соединение закрыто"; return false; }
        }
        std::vector<uint8_t> pending;
        {
            std::lock_guard lock(stream->tcpMutex);
            stream->applicationReady = true;
            pending.swap(stream->pendingData);
        }
        if (!pending.empty() && !sendSocketAll(socket, pending.data(), pending.size())) return false;
        char buffer[16 * 1024];
        for (;;) {
            int count = ::recv(socket, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            if (!send(stream, std::vector<uint8_t>(buffer, buffer + count), QUIC_SEND_FLAG_NONE)) {
                error = runtimeFailed ? L"Соединение Hysteria2 потеряно — переподключитесь" : L"Не удалось отправить данные в Hysteria2 TCP stream";
                return false;
            }
        }
        if (!stopping && api && stream->handle)
            api->StreamShutdown(stream->handle, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        {
            std::lock_guard lock(stream->tcpMutex); stream->socket = INVALID_SOCKET;
        }
        return true;
    }
};

struct lsqpack_dec_hset_if HysteriaClient::Impl::decoderCallbacks = {
    HysteriaClient::Impl::decoderUnblocked,
    HysteriaClient::Impl::decoderPrepare,
    HysteriaClient::Impl::decoderProcess,
};

HysteriaClient::HysteriaClient(std::unique_ptr<Impl> implementation) : implementation_(std::move(implementation)) {}
HysteriaClient::~HysteriaClient() = default;

std::unique_ptr<HysteriaClient> HysteriaClient::connect(const std::wstring& uri, HysteriaConnectResult& result) {
    std::wstring error;
    auto config = parseConfig(uri, error);
    if (!config) { result.message = std::move(error); return nullptr; }
    auto implementation = std::make_unique<Impl>(std::move(*config));
    if (!implementation->initialize(error)) { result.message = std::move(error); return nullptr; }
    if (!implementation->waitForAuth(result)) return nullptr;
    return std::unique_ptr<HysteriaClient>(new HysteriaClient(std::move(implementation)));
}

void HysteriaClient::stop() {
    if (implementation_) implementation_->stop();
}

bool HysteriaClient::relayTcp(const std::string& destination, std::uintptr_t socket, std::wstring& error, bool socksReply) {
    bool ok = implementation_ && implementation_->relayTcp(destination, static_cast<SOCKET>(socket), error, socksReply);
    if (!ok && implementation_) {
        ErrorHandler handler;
        { std::lock_guard lock(implementation_->udpMutex); handler = implementation_->errorHandler; }
        if (handler && !implementation_->stopping) handler(error);
    }
    return ok;
}

bool HysteriaClient::sendUdp(uint32_t sessionId, const std::string& destination, const unsigned char* data, size_t length, std::wstring& error) {
    bool ok = implementation_ && implementation_->sendUdp(sessionId, destination, data, length, error);
    if (!ok && implementation_) {
        ErrorHandler handler;
        { std::lock_guard lock(implementation_->udpMutex); handler = implementation_->errorHandler; }
        if (handler && !implementation_->stopping) handler(error);
    }
    return ok;
}

void HysteriaClient::setUdpReceiveHandler(UdpReceiveHandler handler) {
    if (!implementation_) return;
    std::lock_guard lock(implementation_->udpMutex);
    implementation_->udpHandler = std::move(handler);
}

void HysteriaClient::setErrorHandler(ErrorHandler handler) {
    if (!implementation_) return;
    std::lock_guard lock(implementation_->udpMutex);
    implementation_->errorHandler = std::move(handler);
}

std::wstring HysteriaClient::udpDiagnostics() const {
    return implementation_ ? implementation_->udpDiagnostics() : L"unavailable";
}
