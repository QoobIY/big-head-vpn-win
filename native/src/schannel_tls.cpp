#include "schannel_tls.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>

namespace {
std::wstring socketError(const wchar_t* action, int code = WSAGetLastError()) {
    return std::wstring(action) + L" (Winsock " + std::to_wstring(code) + L")";
}

std::wstring securityError(const wchar_t* action, SECURITY_STATUS code) {
    wchar_t value[16]{};
    swprintf(value, std::size(value), L"%08lX", static_cast<unsigned long>(code));
    return std::wstring(action) + L" (Schannel 0x" + value + L")";
}
}

SchannelTls::~SchannelTls() { close(); }

bool SchannelTls::sendRaw(const void* input, size_t size, std::wstring& error) {
    auto* data = static_cast<const char*>(input);
    while (size) {
        int sent = ::send(socket_, data, static_cast<int>(std::min<size_t>(size, INT_MAX)), 0);
        if (sent <= 0) { error = socketError(L"TLS: не удалось отправить данные"); return false; }
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool SchannelTls::receiveEncrypted(std::wstring& error) {
    std::array<unsigned char, 16384> buffer{};
    int received = ::recv(socket_, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
    if (received == 0) { error = L"TLS: сервер закрыл соединение"; return false; }
    if (received < 0) { error = socketError(L"TLS: ошибка чтения"); return false; }
    encrypted_.insert(encrypted_.end(), buffer.begin(), buffer.begin() + received);
    return true;
}

bool SchannelTls::connect(const std::wstring& host, unsigned short port, std::wstring& error) {
    close();
    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfoW* addresses{};
    std::wstring service = std::to_wstring(port);
    int resolved = GetAddrInfoW(host.c_str(), service.c_str(), &hints, &addresses);
    if (resolved != 0) { error = socketError(L"TLS: не удалось разрешить адрес", resolved); return false; }
    for (auto* address = addresses; address; address = address->ai_next) {
        socket_ = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_ == INVALID_SOCKET) continue;
        DWORD timeout = 15000;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        if (::connect(socket_, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) break;
        resolved = WSAGetLastError();
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    FreeAddrInfoW(addresses);
    if (socket_ == INVALID_SOCKET) { error = socketError(L"TLS: сервер недоступен", resolved); return false; }

    SCHANNEL_CRED credentials{};
    credentials.dwVersion = SCHANNEL_CRED_VERSION;
    credentials.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    TimeStamp expiry{};
    SECURITY_STATUS status = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W),
        SECPKG_CRED_OUTBOUND, nullptr, &credentials, nullptr, nullptr, &credentials_, &expiry);
    if (status != SEC_E_OK) { error = securityError(L"TLS: создание учётных данных", status); close(); return false; }
    haveCredentials_ = true;

    struct AlpnBuffer {
        DWORD listsSize;
        SEC_APPLICATION_PROTOCOL_NEGOTIATION_EXT extension;
        unsigned short protocolListSize;
        unsigned char protocols[3];
    } alpn{static_cast<DWORD>(sizeof(AlpnBuffer) - sizeof(DWORD)),
        SecApplicationProtocolNegotiationExt_ALPN, 3, {2, 'h', '2'}};
    SecBuffer alpnBuffer{sizeof(alpn), SECBUFFER_APPLICATION_PROTOCOLS, &alpn};
    SecBufferDesc input{SECBUFFER_VERSION, 1, &alpnBuffer};
    DWORD attributes{};
    constexpr DWORD requested = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
        ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

    bool first = true;
    for (;;) {
        SecBuffer outputBuffer{0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc output{SECBUFFER_VERSION, 1, &outputBuffer};
        SecBuffer tokenBuffers[2]{};
        SecBufferDesc tokenInput{};
        SecBufferDesc* currentInput = &input;
        if (!first) {
            if (encrypted_.empty() && !receiveEncrypted(error)) { close(); return false; }
            tokenBuffers[0] = {static_cast<unsigned long>(encrypted_.size()), SECBUFFER_TOKEN, encrypted_.data()};
            tokenBuffers[1] = {0, SECBUFFER_EMPTY, nullptr};
            tokenInput = {SECBUFFER_VERSION, 2, tokenBuffers};
            currentInput = &tokenInput;
        }
        status = InitializeSecurityContextW(&credentials_, first ? nullptr : &context_,
            const_cast<wchar_t*>(host.c_str()), requested, 0, SECURITY_NATIVE_DREP,
            currentInput, 0, &context_, &output, &attributes, &expiry);
#ifdef BIG_HEAD_VPN_TESTING
        fwprintf(stderr, L"schannel_handshake status=0x%08lX input=%zu output=%lu\n",
            static_cast<unsigned long>(status), encrypted_.size(), outputBuffer.cbBuffer);
        fflush(stderr);
#endif
        haveContext_ = true;
        if (outputBuffer.pvBuffer) {
            bool sent = sendRaw(outputBuffer.pvBuffer, outputBuffer.cbBuffer, error);
            FreeContextBuffer(outputBuffer.pvBuffer);
            if (!sent) { close(); return false; }
        }
        if (!first && status != SEC_E_INCOMPLETE_MESSAGE) {
            size_t extra = tokenBuffers[1].BufferType == SECBUFFER_EXTRA ? tokenBuffers[1].cbBuffer : 0;
            if (extra) encrypted_.erase(encrypted_.begin(), encrypted_.end() - static_cast<ptrdiff_t>(extra));
            else encrypted_.clear();
        }
        first = false;
        if (status == SEC_E_OK) break;
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            if (!receiveEncrypted(error)) { close(); return false; }
            continue;
        }
        if (status != SEC_I_CONTINUE_NEEDED) {
            error = securityError(L"TLS handshake завершился ошибкой", status);
            close();
            return false;
        }
    }

    SecPkgContext_ApplicationProtocol negotiated{};
    status = QueryContextAttributesW(&context_, SECPKG_ATTR_APPLICATION_PROTOCOL, &negotiated);
    if (status != SEC_E_OK || negotiated.ProtoNegoStatus != SecApplicationProtocolNegotiationStatus_Success ||
        negotiated.ProtocolIdSize != 2 || negotiated.ProtocolId[0] != 'h' || negotiated.ProtocolId[1] != '2') {
        error = L"VLESS XHTTP: сервер не согласовал HTTP/2 (ALPN h2)";
        close();
        return false;
    }
    status = QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_);
    if (status != SEC_E_OK) { error = securityError(L"TLS: параметры потока недоступны", status); close(); return false; }
    // The 15-second receive timeout is only for the TLS handshake. Leaving
    // it on a persistent HTTP/2 socket kills healthy long-lived responses
    // whenever Codex/Discord has no downlink bytes for 15 seconds.
    DWORD blockingReceive = 0;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&blockingReceive), sizeof(blockingReceive));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char*>(&blockingReceive), sizeof(blockingReceive));
    return true;
}

bool SchannelTls::write(std::span<const unsigned char> input, std::wstring& error) {
    std::lock_guard lock(writeMutex_);
    while (!input.empty()) {
        size_t length = std::min<size_t>(input.size(), sizes_.cbMaximumMessage);
        std::vector<unsigned char> record(sizes_.cbHeader + length + sizes_.cbTrailer);
        std::copy_n(input.data(), length, record.data() + sizes_.cbHeader);
        SecBuffer buffers[4]{{sizes_.cbHeader, SECBUFFER_STREAM_HEADER, record.data()},
            {static_cast<unsigned long>(length), SECBUFFER_DATA, record.data() + sizes_.cbHeader},
            {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + sizes_.cbHeader + length},
            {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc message{SECBUFFER_VERSION, 4, buffers};
        SECURITY_STATUS status = EncryptMessage(&context_, 0, &message, 0);
        if (status != SEC_E_OK) { error = securityError(L"TLS: шифрование не удалось", status); return false; }
        size_t recordSize = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
        if (!sendRaw(record.data(), recordSize, error)) return false;
        input = input.subspan(length);
    }
    return true;
}

bool SchannelTls::read(std::vector<unsigned char>& output, std::wstring& error) {
    output.clear();
    for (;;) {
        if (encrypted_.empty() && !receiveEncrypted(error)) return false;
        SecBuffer buffers[4]{{static_cast<unsigned long>(encrypted_.size()), SECBUFFER_DATA, encrypted_.data()},
            {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc message{SECBUFFER_VERSION, 4, buffers};
        SECURITY_STATUS status = DecryptMessage(&context_, &message, 0, nullptr);
        if (status == SEC_E_INCOMPLETE_MESSAGE) { if (!receiveEncrypted(error)) return false; continue; }
        if (status == SEC_I_CONTEXT_EXPIRED) { error = L"TLS: сервер завершил соединение"; return false; }
        if (status != SEC_E_OK) { error = securityError(L"TLS: расшифровка не удалась", status); return false; }
        size_t extra{};
        for (const auto& buffer : buffers) {
            if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer)
                output.assign(static_cast<unsigned char*>(buffer.pvBuffer), static_cast<unsigned char*>(buffer.pvBuffer) + buffer.cbBuffer);
            else if (buffer.BufferType == SECBUFFER_EXTRA) extra = buffer.cbBuffer;
        }
        if (extra) encrypted_.erase(encrypted_.begin(), encrypted_.end() - static_cast<ptrdiff_t>(extra));
        else encrypted_.clear();
        if (!output.empty()) return true;
    }
}

void SchannelTls::close() {
    shutdownTransport();
    if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; }
    if (haveContext_) { DeleteSecurityContext(&context_); haveContext_ = false; }
    if (haveCredentials_) { FreeCredentialsHandle(&credentials_); haveCredentials_ = false; }
    encrypted_.clear();
}

void SchannelTls::shutdownTransport() {
    if (socket_ != INVALID_SOCKET) shutdown(socket_, SD_BOTH);
}
