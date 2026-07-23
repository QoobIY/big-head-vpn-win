#pragma once

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct RealityTlsConfig {
    std::wstring endpoint;
    unsigned short port{};
    std::wstring serverName;
    std::array<unsigned char, 32> publicKey{};
    std::array<unsigned char, 8> shortId{};
    std::vector<std::string> alpn;
};

bool parseRealityTlsConfig(const std::wstring& uri, RealityTlsConfig& config,
    std::wstring& error);

class RealityTls {
public:
    RealityTls();
    ~RealityTls();
    RealityTls(const RealityTls&) = delete;
    RealityTls& operator=(const RealityTls&) = delete;

    bool connect(const RealityTlsConfig& config, std::wstring& error);
    bool write(std::span<const unsigned char> data, std::wstring& error);
    bool read(std::vector<unsigned char>& data, std::wstring& error);
    bool readRaw(std::vector<unsigned char>& data, std::wstring& error);
    void shutdownTransport();
    void close();
    std::string negotiatedAlpn() const;

private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};
