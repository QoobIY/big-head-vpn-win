#pragma once

#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include <winsock2.h>
#include <windows.h>
#include <schannel.h>
#include <security.h>

#include <mutex>
#include <span>
#include <string>
#include <vector>

class SchannelTls {
public:
    SchannelTls() = default;
    ~SchannelTls();
    SchannelTls(const SchannelTls&) = delete;
    SchannelTls& operator=(const SchannelTls&) = delete;

    bool connect(const std::wstring& host, unsigned short port, std::wstring& error);
    bool write(std::span<const unsigned char> data, std::wstring& error);
    bool read(std::vector<unsigned char>& data, std::wstring& error);
    void shutdownTransport();
    void close();

private:
    bool receiveEncrypted(std::wstring& error);
    bool sendRaw(const void* data, size_t size, std::wstring& error);

    SOCKET socket_{INVALID_SOCKET};
    CredHandle credentials_{};
    CtxtHandle context_{};
    bool haveCredentials_{};
    bool haveContext_{};
    SecPkgContext_StreamSizes sizes_{};
    std::vector<unsigned char> encrypted_;
    std::mutex writeMutex_;
};
