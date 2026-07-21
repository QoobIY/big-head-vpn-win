#pragma once

#include <memory>
#include <string>

class HysteriaClient;

class SocksServer {
public:
    static std::unique_ptr<SocksServer> start(
        const std::wstring& address,
        unsigned short port,
        HysteriaClient& client,
        std::wstring& error);

    ~SocksServer();
    SocksServer(const SocksServer&) = delete;
    SocksServer& operator=(const SocksServer&) = delete;

private:
    struct Impl;
    explicit SocksServer(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};
