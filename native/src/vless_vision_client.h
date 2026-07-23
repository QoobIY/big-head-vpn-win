#pragma once

#include "tunnel_client.h"

#include <memory>

class VlessVisionClient final : public TunnelClient {
public:
    static std::unique_ptr<VlessVisionClient> connect(const std::wstring& uri,
        TunnelConnectResult& result);
    ~VlessVisionClient() override;
    VlessVisionClient(const VlessVisionClient&) = delete;
    VlessVisionClient& operator=(const VlessVisionClient&) = delete;

    void stop() override;
    bool relayTcp(const std::string&, std::uintptr_t, std::wstring&, bool = true) override;
    bool sendUdp(uint32_t, const std::string&, const unsigned char*, size_t,
        std::wstring& error) override;
    void setErrorHandler(ErrorHandler handler) override;
    std::wstring udpDiagnostics() const override;
    bool supportsUdp() const override { return false; }

private:
    void installUdpReceiveHandler(UdpReceiveHandler handler) override;
    struct Impl;
    explicit VlessVisionClient(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};
