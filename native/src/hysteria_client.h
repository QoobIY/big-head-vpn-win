#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

#ifdef BIG_HEAD_VPN_TESTING
std::vector<unsigned char> hysteriaAuthFixtureForTest();
#endif

struct HysteriaConnectResult {
    bool connected{};
    bool udpEnabled{};
    std::wstring message;
};

class HysteriaClient {
public:
    static std::unique_ptr<HysteriaClient> connect(
        const std::wstring& uri,
        HysteriaConnectResult& result);

    ~HysteriaClient();
    HysteriaClient(const HysteriaClient&) = delete;
    HysteriaClient& operator=(const HysteriaClient&) = delete;

    void stop();
    bool relayTcp(const std::string& destination, std::uintptr_t socket, std::wstring& error);

private:
    struct Impl;
    explicit HysteriaClient(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};
