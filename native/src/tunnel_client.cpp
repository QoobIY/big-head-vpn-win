#include "tunnel_client.h"
#include "hysteria_client.h"
#include "vless_client.h"

std::unique_ptr<TunnelClient> connectTunnel(const std::wstring& uri, TunnelConnectResult& result) {
    if (uri.rfind(L"hysteria2://", 0) == 0 || uri.rfind(L"hy2://", 0) == 0)
        return HysteriaClient::connect(uri, result);
    if (uri.rfind(L"vless://", 0) == 0)
        return VlessClient::connect(uri, result);
    result.message = L"Этот протокол пока не поддерживается";
    return nullptr;
}
