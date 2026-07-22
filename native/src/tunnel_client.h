#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct TunnelConnectResult {
    bool connected{};
    bool udpEnabled{};
    std::wstring message;
};

class TunnelClient {
public:
    using UdpReceiveHandler = std::function<void(uint32_t, const std::string&, std::vector<unsigned char>)>;
    using ErrorHandler = std::function<void(std::wstring)>;
    virtual ~TunnelClient() = default;
    virtual void stop() = 0;
    virtual bool relayTcp(const std::string& destination, std::uintptr_t socket, std::wstring& error, bool socksReply = true) = 0;
    virtual bool sendUdp(uint32_t sessionId, const std::string& destination,
        const unsigned char* data, size_t length, std::wstring& error) = 0;
    virtual void setUdpReceiveHandler(UdpReceiveHandler handler) = 0;
    virtual void setErrorHandler(ErrorHandler handler) = 0;
    virtual std::wstring udpDiagnostics() const = 0;
    virtual bool supportsUdp() const = 0;
};

std::unique_ptr<TunnelClient> connectTunnel(const std::wstring& uri, TunnelConnectResult& result);
