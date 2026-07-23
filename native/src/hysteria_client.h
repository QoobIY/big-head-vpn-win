#pragma once
#include "tunnel_client.h"

#include <memory>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifdef BIG_HEAD_VPN_TESTING
std::vector<unsigned char> hysteriaAuthFixtureForTest();
#endif

using HysteriaConnectResult = TunnelConnectResult;

class HysteriaClient : public TunnelClient {
public:
    static std::unique_ptr<HysteriaClient> connect(
        const std::wstring& uri,
        HysteriaConnectResult& result);

    ~HysteriaClient() override;
    HysteriaClient(const HysteriaClient&) = delete;
    HysteriaClient& operator=(const HysteriaClient&) = delete;

    void stop() override;
    bool relayTcp(const std::string& destination, std::uintptr_t socket, std::wstring& error, bool socksReply = true) override;
    bool sendUdp(uint32_t sessionId, const std::string& destination, const unsigned char* data, size_t length, std::wstring& error) override;
    void setErrorHandler(ErrorHandler handler) override;
    std::wstring udpDiagnostics() const override;
    bool supportsUdp() const override { return true; }

private:
    void installUdpReceiveHandler(UdpReceiveHandler handler) override;
    struct Impl;
    explicit HysteriaClient(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};
