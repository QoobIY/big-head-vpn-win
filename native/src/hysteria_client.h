#pragma once

#include <memory>
#include <cstdint>
#include <functional>
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
    using UdpReceiveHandler = std::function<void(uint32_t, const std::string&, std::vector<unsigned char>)>;
    static std::unique_ptr<HysteriaClient> connect(
        const std::wstring& uri,
        HysteriaConnectResult& result);

    ~HysteriaClient();
    HysteriaClient(const HysteriaClient&) = delete;
    HysteriaClient& operator=(const HysteriaClient&) = delete;

    void stop();
    bool relayTcp(const std::string& destination, std::uintptr_t socket, std::wstring& error, bool socksReply = true);
    bool sendUdp(uint32_t sessionId, const std::string& destination, const unsigned char* data, size_t length, std::wstring& error);
    void setUdpReceiveHandler(UdpReceiveHandler handler);
    std::wstring udpDiagnostics() const;

private:
    struct Impl;
    explicit HysteriaClient(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};
