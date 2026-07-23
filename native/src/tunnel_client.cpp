#include "tunnel_client.h"
#include "hysteria_client.h"
#include "vless_client.h"
#include "vless_grpc_client.h"
#include "vless_vision_client.h"

#include <algorithm>

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
            lower.find(L"security=reality") != std::wstring::npos &&
            lower.find(L"flow=xtls-rprx-vision") != std::wstring::npos)
            return VlessVisionClient::connect(uri, result);
        return VlessClient::connect(uri, result);
    }
    result.message = L"Этот протокол пока не поддерживается";
    return nullptr;
}
