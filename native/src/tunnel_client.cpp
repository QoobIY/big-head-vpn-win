#include "tunnel_client.h"
#include "hysteria_client.h"
#include "vless_client.h"
#include "vless_grpc_client.h"
#include "vless_vision_client.h"

#include <algorithm>

TunnelClient::UdpHandlerToken TunnelClient::addUdpReceiveHandler(
    UdpReceiveHandler handler) {
    if (!handler) return 0;
    auto slot = std::make_shared<UdpHandlerSlot>();
    slot->handler = std::move(handler);
    UdpHandlerToken token{};
    {
        std::lock_guard lock(udpHandlersMutex_);
        token = nextUdpHandlerToken_++;
        if (!token) token = nextUdpHandlerToken_++;
        const bool install = udpHandlers_.empty();
        udpHandlers_.emplace(token, std::move(slot));
        if (install) {
            installUdpReceiveHandler(
                [this](uint32_t sessionId, const std::string& destination,
                    std::vector<unsigned char> payload) {
                    dispatchUdp(sessionId, destination, std::move(payload));
                });
        }
    }
    return token;
}

void TunnelClient::removeUdpReceiveHandler(UdpHandlerToken token) {
    if (!token) return;
    std::shared_ptr<UdpHandlerSlot> removed;
    {
        std::lock_guard lock(udpHandlersMutex_);
        auto found = udpHandlers_.find(token);
        if (found == udpHandlers_.end()) return;
        removed = found->second;
        udpHandlers_.erase(found);
    }
    {
        // Waiting for this lock guarantees that a callback which captured an
        // owner object has returned before that owner continues destruction.
        std::lock_guard lock(removed->mutex);
        removed->handler = {};
    }
    {
        std::lock_guard lock(udpHandlersMutex_);
        if (udpHandlers_.empty()) installUdpReceiveHandler({});
    }
}

void TunnelClient::dispatchUdp(uint32_t sessionId,
    const std::string& destination, std::vector<unsigned char> payload) {
    std::vector<std::shared_ptr<UdpHandlerSlot>> handlers;
    {
        std::lock_guard lock(udpHandlersMutex_);
        handlers.reserve(udpHandlers_.size());
        for (const auto& [token, handler] : udpHandlers_) {
            (void)token;
            handlers.push_back(handler);
        }
    }
    for (const auto& slot : handlers) {
        std::lock_guard lock(slot->mutex);
        if (slot->handler)
            slot->handler(sessionId, destination, payload);
    }
}

std::unique_ptr<TunnelClient> connectTunnel(const std::wstring& uri, TunnelConnectResult& result) {
    if (uri.rfind(L"hysteria2://", 0) == 0 || uri.rfind(L"hy2://", 0) == 0)
        return HysteriaClient::connect(uri, result);
    if (uri.rfind(L"vless://", 0) == 0) {
        std::wstring lower = uri;
        std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
        if (lower.find(L"type=grpc") != std::wstring::npos &&
            lower.find(L"security=reality") != std::wstring::npos)
            return VlessGrpcClient::connect(uri, result);
        if ((lower.find(L"type=tcp") != std::wstring::npos ||
                lower.find(L"type=raw") != std::wstring::npos) &&
            lower.find(L"security=reality") != std::wstring::npos)
            return VlessVisionClient::connect(uri, result);
        return VlessClient::connect(uri, result);
    }
    result.message = L"Этот протокол пока не поддерживается";
    return nullptr;
}
