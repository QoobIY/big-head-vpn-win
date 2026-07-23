#include "reality_tls.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/debug.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string_view>

namespace {
int hexValue(wchar_t value) {
    if (value >= L'0' && value <= L'9') return value - L'0';
    if (value >= L'a' && value <= L'f') return value - L'a' + 10;
    if (value >= L'A' && value <= L'F') return value - L'A' + 10;
    return -1;
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
        bytes.push_back(value[i] == L'+' ? ' ' : static_cast<char>(value[i]));
    }
    if (bytes.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring output(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), output.data(), length);
    return output;
}

std::wstring queryValue(const std::wstring& uri, std::wstring_view wanted) {
    size_t begin = uri.find(L'?');
    if (begin == std::wstring::npos) return {};
    ++begin;
    size_t limit = uri.find(L'#', begin);
    if (limit == std::wstring::npos) limit = uri.size();
    while (begin < limit) {
        size_t end = std::min(uri.find(L'&', begin), limit);
        if (end == std::wstring::npos) end = limit;
        std::wstring_view field(uri.data() + begin, end - begin);
        size_t equals = field.find(L'=');
        std::wstring name(field.substr(0, equals));
        std::transform(name.begin(), name.end(), name.begin(), towlower);
        if (name == wanted && equals != std::wstring_view::npos)
            return urlDecode(field.substr(equals + 1));
        begin = end + 1;
    }
    return {};
}

bool decodeBase64Url(std::wstring_view input, unsigned char* output, size_t expected) {
    auto digit = [](wchar_t value) -> int {
        if (value >= L'A' && value <= L'Z') return value - L'A';
        if (value >= L'a' && value <= L'z') return value - L'a' + 26;
        if (value >= L'0' && value <= L'9') return value - L'0' + 52;
        if (value == L'-') return 62;
        if (value == L'_') return 63;
        return -1;
    };
    unsigned value = 0, bits = 0;
    size_t written = 0;
    for (wchar_t character : input) {
        if (character == L'=') break;
        int decoded = digit(character);
        if (decoded < 0) return false;
        value = (value << 6U) | static_cast<unsigned>(decoded);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written >= expected) return false;
            output[written++] = static_cast<unsigned char>(value >> bits);
            value &= (1U << bits) - 1U;
        }
    }
    return written == expected;
}

std::wstring mbedError(const wchar_t* action, int code) {
    std::array<char, 256> detail{};
    mbedtls_strerror(code, detail.data(), detail.size());
    int length = MultiByteToWideChar(CP_UTF8, 0, detail.data(), -1, nullptr, 0);
    std::wstring converted(length > 0 ? static_cast<size_t>(length) : 0, L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, detail.data(), -1, converted.data(), length);
        if (!converted.empty() && converted.back() == L'\0') converted.pop_back();
    }
    return std::wstring(action) + L" (" + std::to_wstring(code) + L"): " + converted;
}

bool derLength(const unsigned char*& cursor, const unsigned char* end, size_t& length) {
    if (cursor >= end) return false;
    unsigned char first = *cursor++;
    if ((first & 0x80U) == 0) { length = first; return static_cast<size_t>(end - cursor) >= length; }
    size_t bytes = first & 0x7fU;
    if (!bytes || bytes > sizeof(size_t) || static_cast<size_t>(end - cursor) < bytes) return false;
    length = 0;
    for (size_t i = 0; i < bytes; ++i) length = (length << 8U) | *cursor++;
    return static_cast<size_t>(end - cursor) >= length;
}

bool derElement(const unsigned char*& cursor, const unsigned char* end,
    unsigned char tag, const unsigned char*& value, size_t& length) {
    if (cursor >= end || *cursor++ != tag || !derLength(cursor, end, length)) return false;
    value = cursor;
    cursor += length;
    return true;
}

bool constantTimeEqual(const unsigned char* left, const unsigned char* right, size_t length) {
    unsigned difference = 0;
    for (size_t i = 0; i < length; ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}
}

bool parseRealityTlsConfig(const std::wstring& uri, RealityTlsConfig& config,
    std::wstring& error) {
    if (uri.rfind(L"vless://", 0) != 0) { error = L"REALITY: нужна VLESS-ссылка"; return false; }
    size_t at = uri.find(L'@'), query = uri.find(L'?', at);
    if (at == std::wstring::npos || query == std::wstring::npos) {
        error = L"REALITY: в ссылке нет адреса или параметров"; return false;
    }
    std::wstring_view authority(uri.data() + at + 1, query - at - 1);
    size_t colon = authority.rfind(L':');
    if (colon == std::wstring_view::npos) { error = L"REALITY: не указан порт"; return false; }
    config.endpoint.assign(authority.substr(0, colon));
    wchar_t* portEnd{};
    std::wstring portText(authority.substr(colon + 1));
    unsigned long port = wcstoul(portText.c_str(), &portEnd, 10);
    if (!portEnd || *portEnd || !port || port > 65535) { error = L"REALITY: неверный порт"; return false; }
    config.port = static_cast<unsigned short>(port);
    std::wstring security = queryValue(uri, L"security");
    std::transform(security.begin(), security.end(), security.begin(), towlower);
    if (security != L"reality") { error = L"Профиль не использует REALITY"; return false; }
    config.serverName = queryValue(uri, L"sni");
    if (config.serverName.empty()) config.serverName = config.endpoint;
    std::wstring publicKey = queryValue(uri, L"pbk");
    if (publicKey.empty()) publicKey = queryValue(uri, L"password");
    if (!decodeBase64Url(publicKey, config.publicKey.data(), config.publicKey.size())) {
        error = L"REALITY: некорректный публичный ключ"; return false;
    }
    std::wstring shortId = queryValue(uri, L"sid");
    if (shortId.size() > 16 || shortId.size() % 2) { error = L"REALITY: некорректный short ID"; return false; }
    config.shortId.fill(0);
    for (size_t i = 0; i < shortId.size(); i += 2) {
        int high = hexValue(shortId[i]), low = hexValue(shortId[i + 1]);
        if (high < 0 || low < 0) { error = L"REALITY: некорректный short ID"; return false; }
        config.shortId[i / 2] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

struct RealityTls::Impl {
    SOCKET socket{INVALID_SOCKET};
    mbedtls_ssl_context ssl{};
    mbedtls_ssl_config sslConfig{};
    mbedtls_entropy_context entropy{};
    mbedtls_ctr_drbg_context random{};
    RealityTlsConfig reality;
    std::array<unsigned char, 32> authKey{};
    bool verified{};
    bool initialized{};
    std::atomic_bool transportEof{};
    std::vector<std::string> alpnStorage;
    std::vector<const char*> alpnPointers;
    std::mutex writeMutex;

    Impl() {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&sslConfig);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&random);
        initialized = true;
    }

    ~Impl() { close(); }

    static int sendCallback(void* context, const unsigned char* data, size_t length) {
        auto* self = static_cast<Impl*>(context);
        int sent = ::send(self->socket, reinterpret_cast<const char*>(data),
            static_cast<int>(std::min<size_t>(length, INT_MAX)), 0);
        if (sent >= 0) return sent;
        int code = WSAGetLastError();
        return code == WSAEWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    }

#ifdef BIG_HEAD_VPN_TESTING
    static void debugCallback(void*, int, const char* file, int line, const char* message) {
        std::fprintf(stderr, "mbedtls %s:%d %s", file, line, message);
    }
#endif

    static int receiveCallback(void* context, unsigned char* data, size_t length) {
        auto* self = static_cast<Impl*>(context);
        int received = recv(self->socket, reinterpret_cast<char*>(data),
            static_cast<int>(std::min<size_t>(length, INT_MAX)), 0);
        if (received > 0) { self->transportEof = false; return received; }
        if (received == 0) { self->transportEof = true; return MBEDTLS_ERR_SSL_CONN_EOF; }
        int code = WSAGetLastError();
        if (code == WSAECONNABORTED || code == WSAECONNRESET || code == WSAESHUTDOWN) {
            self->transportEof = true;
            return MBEDTLS_ERR_SSL_CONN_EOF;
        }
        return code == WSAEWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    }

    static int transformClientHello(void* context, mbedtls_svc_key_id_t privateKey,
        const unsigned char randomBytes[32], unsigned char* body, size_t bodyLength) {
        auto* self = static_cast<Impl*>(context);
        if (bodyLength < 67 || body[34] != 32) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
        std::fill_n(body + 35, 32, 0);
        std::vector<unsigned char> authenticated(4 + bodyLength);
        authenticated[0] = 1;
        authenticated[1] = static_cast<unsigned char>(bodyLength >> 16U);
        authenticated[2] = static_cast<unsigned char>(bodyLength >> 8U);
        authenticated[3] = static_cast<unsigned char>(bodyLength);
        std::copy_n(body, bodyLength, authenticated.data() + 4);

        std::array<unsigned char, 32> shared{};
        size_t sharedLength{};
        psa_status_t status = psa_raw_key_agreement(PSA_ALG_ECDH, privateKey,
            self->reality.publicKey.data(), self->reality.publicKey.size(),
            shared.data(), shared.size(), &sharedLength);
        if (status != PSA_SUCCESS || sharedLength != shared.size()) return MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE;
        const mbedtls_md_info_t* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        static constexpr unsigned char info[]{'R','E','A','L','I','T','Y'};
        if (!sha256 || mbedtls_hkdf(sha256, randomBytes, 20, shared.data(), shared.size(),
                info, sizeof(info), self->authKey.data(), self->authKey.size()) != 0)
            return MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE;

        std::array<unsigned char, 16> request{};
        request[0] = 26; request[1] = 3; request[2] = 27;
        uint32_t now = static_cast<uint32_t>(std::time(nullptr));
        request[4] = static_cast<unsigned char>(now >> 24U);
        request[5] = static_cast<unsigned char>(now >> 16U);
        request[6] = static_cast<unsigned char>(now >> 8U);
        request[7] = static_cast<unsigned char>(now);
        std::copy(self->reality.shortId.begin(), self->reality.shortId.end(), request.begin() + 8);
        mbedtls_gcm_context gcm{};
        mbedtls_gcm_init(&gcm);
        int result = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES,
            self->authKey.data(), 256);
        if (result == 0) result = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
            request.size(), randomBytes + 20, 12, authenticated.data(), authenticated.size(),
            request.data(), body + 35, 16, body + 51);
        mbedtls_gcm_free(&gcm);
        return result;
    }

    static int authenticateCertificate(void* context, const unsigned char* der, size_t length) {
        auto* self = static_cast<Impl*>(context);
        const unsigned char* cursor = der;
        const unsigned char* end = der + length;
        const unsigned char* certificate{};
        size_t certificateLength{};
        if (!derElement(cursor, end, 0x30, certificate, certificateLength) || cursor != end) return -1;
        const unsigned char* part = certificate;
        const unsigned char* certificateEnd = certificate + certificateLength;
        const unsigned char* value{};
        size_t valueLength{};
        if (!derElement(part, certificateEnd, 0x30, value, valueLength) ||
            !derElement(part, certificateEnd, 0x30, value, valueLength) ||
            !derElement(part, certificateEnd, 0x03, value, valueLength) ||
            part != certificateEnd || valueLength != 65 || value[0] != 0) return -1;
        const unsigned char* signature = value + 1;

        static constexpr unsigned char ed25519Oid[]{0x06, 0x03, 0x2b, 0x65, 0x70};
        const unsigned char* oid = std::search(der, end, std::begin(ed25519Oid), std::end(ed25519Oid));
        if (oid == end) return -1;
        const unsigned char* key = oid + sizeof(ed25519Oid);
        while (key + 35 <= end && !(key[0] == 0x03 && key[1] == 0x21 && key[2] == 0x00)) ++key;
        if (key + 35 > end) return -1;
        key += 3;

        std::array<unsigned char, 64> expected{};
        const mbedtls_md_info_t* sha512 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
        if (!sha512 || mbedtls_md_hmac(sha512, self->authKey.data(), self->authKey.size(),
                key, 32, expected.data()) != 0) return -1;
        if (!constantTimeEqual(expected.data(), signature, expected.size())) return -1;
        self->verified = true;
        return 0;
    }

    bool openSocket(std::wstring& error) {
        addrinfoW hints{};
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
        addrinfoW* addresses{};
        int result = GetAddrInfoW(reality.endpoint.c_str(), std::to_wstring(reality.port).c_str(),
            &hints, &addresses);
        if (result != 0) { error = L"REALITY: не удалось разрешить адрес"; return false; }
        int lastError = result;
        for (auto* address = addresses; address; address = address->ai_next) {
            socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (socket == INVALID_SOCKET) continue;
            DWORD timeout = 15000;
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            if (::connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) break;
            lastError = WSAGetLastError(); closesocket(socket); socket = INVALID_SOCKET;
        }
        FreeAddrInfoW(addresses);
        if (socket == INVALID_SOCKET) {
            error = L"REALITY: сервер недоступен (Winsock " + std::to_wstring(lastError) + L")";
            return false;
        }
        return true;
    }

    void close() {
        if (socket != INVALID_SOCKET) { shutdown(socket, SD_BOTH); closesocket(socket); socket = INVALID_SOCKET; }
        if (initialized) {
            mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&sslConfig);
            mbedtls_ctr_drbg_free(&random); mbedtls_entropy_free(&entropy);
            initialized = false;
        }
    }
};

RealityTls::RealityTls() : implementation_(std::make_unique<Impl>()) {}
RealityTls::~RealityTls() = default;

bool RealityTls::connect(const RealityTlsConfig& config, std::wstring& error) {
    implementation_ = std::make_unique<Impl>();
    auto& self = *implementation_;
    self.reality = config;
    if (!self.openSocket(error)) return false;
    if (psa_crypto_init() != PSA_SUCCESS) { error = L"REALITY: PSA crypto не запущена"; return false; }
    static constexpr unsigned char personalization[] = "BigHeadVPN REALITY";
    int result = mbedtls_ctr_drbg_seed(&self.random, mbedtls_entropy_func, &self.entropy,
        personalization, sizeof(personalization) - 1);
    if (result == 0) result = mbedtls_ssl_config_defaults(&self.sslConfig,
        MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) { error = mbedError(L"REALITY: инициализация TLS", result); return false; }
    mbedtls_ssl_conf_rng(&self.sslConfig, mbedtls_ctr_drbg_random, &self.random);
#ifdef BIG_HEAD_VPN_TESTING
    if (GetEnvironmentVariableW(L"BHVPN_MBEDTLS_DEBUG", nullptr, 0) > 0) {
        mbedtls_debug_set_threshold(2);
        mbedtls_ssl_conf_dbg(&self.sslConfig, Impl::debugCallback, nullptr);
    }
#endif
    mbedtls_ssl_conf_min_tls_version(&self.sslConfig, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&self.sslConfig, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_authmode(&self.sslConfig, MBEDTLS_SSL_VERIFY_REQUIRED);
    static const uint16_t groups[]{MBEDTLS_SSL_IANA_TLS_GROUP_X25519, MBEDTLS_SSL_IANA_TLS_GROUP_NONE};
    mbedtls_ssl_conf_groups(&self.sslConfig, groups);
    // REALITY first forwards our ClientHello to the camouflage target.  Offering
    // only Ed25519 makes ordinary RSA/ECDSA sites abort before REALITY can
    // replace their certificate.  Keep Ed25519 for REALITY's temporary
    // certificate, but advertise the same broadly-compatible families as a
    // normal browser.
    static const uint16_t signatures[]{
        MBEDTLS_TLS1_3_SIG_ECDSA_SECP256R1_SHA256,
        MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA256,
        MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA256,
        MBEDTLS_TLS1_3_SIG_ED25519,
        MBEDTLS_TLS1_3_SIG_ECDSA_SECP384R1_SHA384,
        MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA384,
        MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA384,
        MBEDTLS_TLS1_3_SIG_RSA_PSS_RSAE_SHA512,
        MBEDTLS_TLS1_3_SIG_RSA_PKCS1_SHA512,
        MBEDTLS_TLS1_3_SIG_ECDSA_SECP521R1_SHA512,
        MBEDTLS_TLS1_3_SIG_NONE};
    mbedtls_ssl_conf_sig_algs(&self.sslConfig, signatures);
    mbedtls_ssl_conf_client_hello_transform(&self.sslConfig, Impl::transformClientHello, &self);
    mbedtls_ssl_conf_server_certificate_auth(&self.sslConfig, Impl::authenticateCertificate, &self);
    if (!config.alpn.empty()) {
        self.alpnStorage = config.alpn;
        for (auto& protocol : self.alpnStorage) self.alpnPointers.push_back(protocol.c_str());
        self.alpnPointers.push_back(nullptr);
        result = mbedtls_ssl_conf_alpn_protocols(&self.sslConfig, self.alpnPointers.data());
        if (result != 0) { error = mbedError(L"REALITY: настройка ALPN", result); return false; }
    }
    result = mbedtls_ssl_setup(&self.ssl, &self.sslConfig);
    if (result == 0) {
        std::string serverName(config.serverName.begin(), config.serverName.end());
        result = mbedtls_ssl_set_hostname(&self.ssl, serverName.c_str());
    }
    if (result != 0) { error = mbedError(L"REALITY: настройка TLS", result); return false; }
    mbedtls_ssl_set_bio(&self.ssl, &self, Impl::sendCallback, Impl::receiveCallback, nullptr);
    do { result = mbedtls_ssl_handshake(&self.ssl); }
    while (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (result != 0 || !self.verified) {
        error = result != 0 ? mbedError(L"REALITY handshake", result) : L"REALITY: сертификат сервера не подтверждён";
        return false;
    }
    DWORD noTimeout = 0;
    setsockopt(self.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&noTimeout), sizeof(noTimeout));
    setsockopt(self.socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&noTimeout), sizeof(noTimeout));
    return true;
}

bool RealityTls::write(std::span<const unsigned char> data, std::wstring& error) {
    if (!implementation_) { error = L"REALITY уже закрыт"; return false; }
    std::lock_guard lock(implementation_->writeMutex);
    while (!data.empty()) {
        int result = mbedtls_ssl_write(&implementation_->ssl, data.data(), data.size());
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (result <= 0) { error = mbedError(L"REALITY: ошибка записи", result); return false; }
        data = data.subspan(static_cast<size_t>(result));
    }
    return true;
}

bool RealityTls::read(std::vector<unsigned char>& data, std::wstring& error) {
    data.resize(16384);
    for (;;) {
        int result = mbedtls_ssl_read(&implementation_->ssl, data.data(), data.size());
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        // TLS 1.3 post-handshake messages (notably NewSessionTicket) may be
        // consumed internally and yield zero application bytes. This is not
        // EOF unless the underlying recv() actually returned zero.
        if (result == 0 && !implementation_->transportEof) continue;
        if (result == MBEDTLS_ERR_SSL_CONN_EOF || (result == 0 && implementation_->transportEof)) {
            data.clear();
            error = L"REALITY: сервер закрыл соединение";
            return false;
        }
        if (result <= 0) { data.clear(); error = mbedError(L"REALITY: ошибка чтения", result); return false; }
        data.resize(static_cast<size_t>(result));
        return true;
    }
}

bool RealityTls::readRaw(std::vector<unsigned char>& data, std::wstring& error) {
    if (!implementation_ || implementation_->socket == INVALID_SOCKET) {
        error = L"REALITY: сокет уже закрыт";
        return false;
    }
    data.resize(16384);
    int received = recv(implementation_->socket, reinterpret_cast<char*>(data.data()),
        static_cast<int>(data.size()), 0);
    if (received <= 0) {
        data.clear();
        error = L"REALITY direct: соединение закрыто";
        return false;
    }
    data.resize(static_cast<size_t>(received));
    return true;
}

void RealityTls::shutdownTransport() {
    if (implementation_ && implementation_->socket != INVALID_SOCKET)
        shutdown(implementation_->socket, SD_BOTH);
}

void RealityTls::close() { if (implementation_) implementation_->close(); }

std::string RealityTls::negotiatedAlpn() const {
    if (!implementation_) return {};
    const char* protocol = mbedtls_ssl_get_alpn_protocol(&implementation_->ssl);
    return protocol ? protocol : "";
}
