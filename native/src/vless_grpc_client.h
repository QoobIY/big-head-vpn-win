#pragma once

#include "tunnel_client.h"

#include <memory>

class VlessGrpcClient final : public TunnelClient {
public:
    static std::unique_ptr<VlessGrpcClient> connect(const std::wstring& uri,
        TunnelConnectResult& result);
    ~VlessGrpcClient() override;
    VlessGrpcClient(const VlessGrpcClient&) = delete;
    VlessGrpcClient& operator=(const VlessGrpcClient&) = delete;

    void stop() override;
    bool relayTcp(const std::string& destination, std::uintptr_t socket,
        std::wstring& error, bool socksReply = true) override;
    bool sendUdp(uint32_t, const std::string&, const unsigned char*, size_t,
        std::wstring& error) override;
    void setUdpReceiveHandler(UdpReceiveHandler) override;
    void setErrorHandler(ErrorHandler handler) override;
    std::wstring udpDiagnostics() const override;
    bool supportsUdp() const override { return true; }

private:
    struct Impl;
    explicit VlessGrpcClient(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};
