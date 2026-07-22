#pragma once

#include "tunnel_client.h"

#include <memory>

class VlessClient final : public TunnelClient {
public:
    static std::unique_ptr<VlessClient> connect(const std::wstring& uri, TunnelConnectResult& result);
    ~VlessClient() override;
    VlessClient(const VlessClient&) = delete;
    VlessClient& operator=(const VlessClient&) = delete;

    void stop() override;
    bool relayTcp(const std::string& destination, std::uintptr_t socket,
        std::wstring& error, bool socksReply = true) override;
    bool sendUdp(uint32_t, const std::string&, const unsigned char*, size_t,
        std::wstring& error) override;
    void setUdpReceiveHandler(UdpReceiveHandler) override;
    std::wstring udpDiagnostics() const override;
    bool supportsUdp() const override { return true; }

private:
    struct Impl;
    explicit VlessClient(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};

#ifdef BIG_HEAD_VPN_TESTING
bool vlessProtocolFixtureForTest(std::wstring& error);
#endif
