#include "hysteria_client.h"
#include "vless_client.h"
#include "tunnel_client.h"
#include "socks_server.h"
#include "process_filter.h"
#include "autostart.h"
#include "schannel_tls.h"
#include "http2_connection.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--vless-fixture") {
        std::wstring error;
        bool passed = vlessProtocolFixtureForTest(error);
        std::cout << (passed ? "vless_fixture_ok" : "vless_fixture_failed ")
                  << (passed ? "" : utf8(error)) << std::endl;
        return passed ? 0 : 1;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--tunnel-udp-uri") {
        TunnelConnectResult result;
        auto client = connectTunnel(argv[2], result);
        if (!client || !client->supportsUdp()) {
            std::cout << "udp_connect_failed " << utf8(result.message) << std::endl;
            return 1;
        }
        std::mutex mutex;
        std::condition_variable changed;
        unsigned received = 0;
        size_t responseLength = 0;
        client->setUdpReceiveHandler([&](uint32_t session, const std::string&, std::vector<unsigned char> payload) {
            if (session != 1) return;
            std::lock_guard lock(mutex);
            ++received;
            responseLength = payload.size();
            changed.notify_all();
        });
        const unsigned char stun[]{0x00,0x01,0x00,0x00,0x21,0x12,0xA4,0x42,
            0x42,0x69,0x67,0x48,0x65,0x61,0x64,0x56,0x50,0x4E,0x30,0x31};
        std::wstring error;
        bool sent = client->sendUdp(1, "stun.l.google.com:19302", stun, sizeof(stun), error);
        auto second = std::to_array(stun);
        second.back() ^= 1;
        sent = sent && client->sendUdp(1, "stun.l.google.com:19302", second.data(), second.size(), error);
        if (sent) {
            std::unique_lock lock(mutex);
            changed.wait_for(lock, std::chrono::seconds(15), [&] { return received >= 2; });
        }
        std::cout << (received >= 2 ? "udp_responses=2 last_bytes=" + std::to_string(responseLength) : "udp_failed ")
                  << (received >= 2 ? "" : utf8(error)) << " " << utf8(client->udpDiagnostics()) << std::endl;
        client->stop();
        return received >= 2 ? 0 : 1;
    }
    if (argc == 4 && (std::wstring_view(argv[1]) == L"--socks-uri" ||
            std::wstring_view(argv[1]) == L"--socks-uri-stop")) {
        bool stopTest = std::wstring_view(argv[1]) == L"--socks-uri-stop";
        TunnelConnectResult result;
        auto client = connectTunnel(argv[2], result);
        if (!client) { std::cout << "connect_failed " << utf8(result.message) << std::endl; return 1; }
        std::wstring error;
        auto server = SocksServer::start(L"127.0.0.1", static_cast<unsigned short>(_wtoi(argv[3])), *client, error);
        if (!server) { std::cout << "listen_failed " << utf8(error) << std::endl; return 1; }
        std::cout << "socks_ready" << std::endl;
        if (stopTest) {
            std::this_thread::sleep_for(std::chrono::seconds(15));
            auto before = std::chrono::steady_clock::now();
            client->stop();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - before).count();
            std::cout << "vless_stop_ms=" << elapsed << std::endl;
        } else std::this_thread::sleep_for(std::chrono::seconds(120));
        return 0;
    }
    if ((argc == 5 && std::wstring_view(argv[1]) == L"--h2-get") ||
        (argc == 6 && std::wstring_view(argv[1]) == L"--h2-idle-get")) {
        bool idle = std::wstring_view(argv[1]) == L"--h2-idle-get";
        WSADATA winsock{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
        Http2Connection http;
        std::wstring error;
        std::string authority = utf8(argv[2]);
        bool connected = http.connect(argv[2], static_cast<unsigned short>(_wtoi(argv[3])), authority, error);
        if (connected && idle) std::this_thread::sleep_for(std::chrono::seconds(_wtoi(argv[5])));
        int32_t stream = connected ? http.open(utf8(argv[4]), {{"accept", "*/*"}}, {}, {}, error) : -1;
        unsigned status{};
        bool answered = stream >= 0 && http.waitHeaders(stream, status, error, 5000);
        std::cout << (answered ? "h2_response status=" + std::to_string(status) : "h2_failed ")
                  << (answered ? "" : utf8(error)) << std::endl;
        http.close();
        WSACleanup();
        return answered ? 0 : 1;
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--tls-h2") {
        WSADATA winsock{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
        SchannelTls tls;
        std::wstring error;
        bool connected = tls.connect(argv[2], static_cast<unsigned short>(_wtoi(argv[3])), error);
        std::cout << (connected ? "tls_h2_ready" : "tls_h2_failed ") << utf8(error) << std::endl;
        tls.close();
        WSACleanup();
        return connected ? 0 : 1;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--fixture") {
        for (unsigned char byte : hysteriaAuthFixtureForTest())
            std::cout << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
        std::cout << '\n';
        return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--autostart-cycle") {
        std::wstring error;
        bool before = isAutostartEnabled(error);
        bool enabled = before || setAutostartEnabled(true, error);
        bool visible = enabled && isAutostartEnabled(error);
        bool cleaned = before || setAutostartEnabled(false, error);
        std::ofstream status(std::filesystem::path(argv[2]), std::ios::trunc);
        status << "before=" << before << " enabled=" << enabled << " visible=" << visible << " cleaned=" << cleaned
               << " error=" << utf8(error) << '\n';
        return enabled && visible && cleaned ? 0 : 1;
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--socks-udp") {
        WSADATA winsock{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
        SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in proxy{}; proxy.sin_family = AF_INET; proxy.sin_port = htons(static_cast<unsigned short>(_wtoi(argv[3])));
        InetPtonW(AF_INET, argv[2], &proxy.sin_addr);
        if (connect(tcp, reinterpret_cast<sockaddr*>(&proxy), sizeof(proxy)) != 0) { std::cout << "socks_connect_failed\n"; return 1; }
        const unsigned char greeting[]{5,1,0}; send(tcp, reinterpret_cast<const char*>(greeting), sizeof(greeting), 0);
        unsigned char answer[64]{}; if (recv(tcp, reinterpret_cast<char*>(answer), 2, MSG_WAITALL) != 2 || answer[1] != 0) return 1;
        const unsigned char associate[]{5,3,0,1,0,0,0,0,0,0}; send(tcp, reinterpret_cast<const char*>(associate), sizeof(associate), 0);
        if (recv(tcp, reinterpret_cast<char*>(answer), 10, MSG_WAITALL) != 10 || answer[1] != 0 || answer[3] != 1) return 1;
        sockaddr_in relay{}; relay.sin_family = AF_INET; relay.sin_addr = proxy.sin_addr;
        std::memcpy(&relay.sin_port, answer + 8, 2);
        SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        DWORD timeout = 10000; setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        const std::string destination = "stun.l.google.com";
        const unsigned char stun[]{0x00,0x01,0x00,0x00,0x21,0x12,0xA4,0x42,0x42,0x69,0x67,0x48,0x65,0x61,0x64,0x56,0x50,0x4E,0x30,0x31};
        std::vector<unsigned char> request{0,0,0,3,static_cast<unsigned char>(destination.size())};
        request.insert(request.end(), destination.begin(), destination.end()); request.push_back(0x4B); request.push_back(0x66);
        request.insert(request.end(), std::begin(stun), std::end(stun));
        sendto(udp, reinterpret_cast<const char*>(request.data()), static_cast<int>(request.size()), 0, reinterpret_cast<sockaddr*>(&relay), sizeof(relay));
        int received = recvfrom(udp, reinterpret_cast<char*>(answer), sizeof(answer), 0, nullptr, nullptr);
        std::cout << (received > 0 ? "socks_udp_response bytes=" + std::to_string(received) : "socks_udp_timeout") << '\n';
        closesocket(udp); closesocket(tcp); WSACleanup(); return received > 0 ? 0 : 1;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--direct-udp") {
        WSADATA winsock{}; if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; hints.ai_protocol = IPPROTO_UDP;
        addrinfo* addresses{};
        if (getaddrinfo("stun.l.google.com", "19302", &hints, &addresses) != 0) return 1;
        SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        DWORD timeout = 10000; setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        const unsigned char stun[]{0x00,0x01,0x00,0x00,0x21,0x12,0xA4,0x42,0x42,0x69,0x67,0x48,0x65,0x61,0x64,0x56,0x50,0x4E,0x30,0x31};
        sendto(udp, reinterpret_cast<const char*>(stun), sizeof(stun), 0, addresses->ai_addr, static_cast<int>(addresses->ai_addrlen));
        unsigned char answer[128]{}; int received = recvfrom(udp, reinterpret_cast<char*>(answer), sizeof(answer), 0, nullptr, nullptr);
        std::cout << (received > 0 ? "direct_udp_response bytes=" + std::to_string(received) : "direct_udp_timeout") << '\n';
        freeaddrinfo(addresses); closesocket(udp); WSACleanup(); return received > 0 ? 0 : 1;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--direct-tcp6") {
        WSADATA winsock{}; if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
        SOCKET tcp = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in6 destination{}; destination.sin6_family = AF_INET6; destination.sin6_port = htons(80);
        InetPtonW(AF_INET6, L"2606:4700:4700::1111", &destination.sin6_addr);
        bool connected = connect(tcp, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == 0;
        const char request[] = "HEAD / HTTP/1.1\r\nHost: one.one.one.one\r\nConnection: close\r\n\r\n";
        if (connected) send(tcp, request, sizeof(request) - 1, 0);
        char answer[64]{}; int received = connected ? recv(tcp, answer, sizeof(answer), 0) : SOCKET_ERROR;
        std::cout << (received > 0 ? "direct_tcp6_response bytes=" + std::to_string(received) : "direct_tcp6_failed error=" + std::to_string(WSAGetLastError())) << '\n';
        closesocket(tcp); WSACleanup(); return received > 0 ? 0 : 1;
    }
    bool udpMode = (argc == 3 || argc == 4) && std::wstring_view(argv[1]) == L"--udp";
    bool socksMode = argc == 4 && std::wstring_view(argv[1]) == L"--socks";
    bool filterMode = (argc == 4 || argc == 5) && std::wstring_view(argv[1]) == L"--filter";
    if (argc != 2 && !udpMode && !socksMode && !filterMode) { std::wcerr << L"usage: BigHeadVPNProbe subscription-file\n"; return 2; }
    std::ifstream input(std::filesystem::path((udpMode || socksMode || filterMode) ? argv[2] : argv[1]), std::ios::binary);
    std::string line;
    std::vector<std::string> profiles;
    while (std::getline(input, line)) {
        if (line.rfind("hysteria2://", 0) == 0 || line.rfind("hy2://", 0) == 0 ||
            (socksMode && line.rfind("vless://", 0) == 0 && line.find("type=xhttp") != std::string::npos)) profiles.push_back(line);
    }
    if (profiles.empty()) { std::wcerr << L"no Hysteria2 profile\n"; return 3; }
    if (udpMode) {
        size_t profileIndex = 0;
        if (argc == 4) {
            wchar_t* end{}; unsigned long requested = wcstoul(argv[3], &end, 10);
            if (!end || *end || requested == 0 || requested > profiles.size()) { std::cout << "invalid_profile\n"; return 2; }
            profileIndex = requested - 1;
        }
        HysteriaConnectResult result; auto client = HysteriaClient::connect(wide(profiles[profileIndex]), result);
        if (!client || !result.udpEnabled) { std::cout << "udp_connect_failed\n"; return 1; }
        std::mutex mutex; std::condition_variable changed; bool received = false; size_t responseLength = 0;
        client->setUdpReceiveHandler([&](uint32_t session, const std::string&, std::vector<unsigned char> payload) {
            if (session != 1U) return; std::lock_guard lock(mutex); received = true; responseLength = payload.size(); changed.notify_all();
        });
        // STUN binding request: unlike public DNS, this destination is not
        // commonly blocked by proxy providers to prevent resolver abuse.
        const unsigned char stun[]{0x00,0x01,0x00,0x00,0x21,0x12,0xA4,0x42,
            0x42,0x69,0x67,0x48,0x65,0x61,0x64,0x56,0x50,0x4E,0x30,0x31};
        std::wstring error;
        if (!client->sendUdp(1U, "stun.l.google.com:19302", stun, sizeof(stun), error)) {
            std::cout << "udp_send_failed " << utf8(error) << " " << utf8(client->udpDiagnostics()) << '\n'; return 1;
        }
        std::unique_lock lock(mutex); changed.wait_for(lock, std::chrono::seconds(10), [&] { return received; });
        std::cout << (received ? "udp_response bytes=" + std::to_string(responseLength) : "udp_timeout")
                  << " " << utf8(client->udpDiagnostics()) << '\n';
        return received ? 0 : 1;
    }
    if (socksMode) {
        TunnelConnectResult result;
        auto client = connectTunnel(wide(profiles.front()), result);
        if (!client) { std::cout << "connect_failed\n"; return 1; }
        wchar_t* end{}; unsigned long port = wcstoul(argv[3], &end, 10);
        std::wstring error;
        auto server = SocksServer::start(L"127.0.0.1", static_cast<unsigned short>(port), *client, error);
        if (!server) { std::cout << "listen_failed\n"; return 1; }
        std::cout << "socks_ready\n" << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(20));
        return 0;
    }
    if (filterMode) {
        HysteriaConnectResult result;
        auto client = HysteriaClient::connect(wide(profiles.front()), result);
        if (!client) { if (argc == 5) { std::ofstream status(std::filesystem::path(argv[4]), std::ios::trunc); status << "connect_failed\n"; } std::cout << "connect_failed\n"; return 1; }
        std::wstring error;
        auto filter = ProcessFilter::start({argv[3]}, *client, error);
        if (!filter) { if (argc == 5) { std::ofstream status(std::filesystem::path(argv[4]), std::ios::trunc); status << "filter_failed code=" << GetLastError() << " message=" << utf8(error) << "\n"; } std::cout << "filter_failed code=" << GetLastError() << "\n"; return 1; }
        if (argc == 5) { std::ofstream status(std::filesystem::path(argv[4]), std::ios::trunc); status << "filter_ready\n"; }
        std::cout << "filter_ready\n" << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(20));
        if (argc == 5) { std::ofstream status(std::filesystem::path(argv[4]), std::ios::app); status << "events=" << filter->socketEvents() << " named=" << filter->namedProcesses() << " matched=" << filter->matchedFlows() << " redirected=" << filter->redirectedPackets() << " accepted=" << filter->acceptedConnections() << " udp_matched=" << filter->udpMatchedFlows() << " udp_sent=" << filter->udpSentPackets() << " udp_received=" << filter->udpReceivedPackets() << " inject_fail=" << filter->injectionFailures() << " inject_error=" << filter->lastInjectionError() << "\n"; }
        std::cout << "matched=" << filter->matchedFlows() << " redirected=" << filter->redirectedPackets() << " accepted=" << filter->acceptedConnections() << " udp_matched=" << filter->udpMatchedFlows() << " udp_sent=" << filter->udpSentPackets() << " udp_received=" << filter->udpReceivedPackets() << "\n";
        return 0;
    }
    bool anyConnected = false;
    for (size_t i = 0; i < profiles.size(); ++i) {
        HysteriaConnectResult result;
        auto client = HysteriaClient::connect(wide(profiles[i]), result);
        std::cout << "profile=" << (i + 1) << ' ' << (result.connected ? "connected" : "failed")
                  << " udp=" << (result.udpEnabled ? "true" : "false")
                  << " message_chars=" << result.message.size()
                  << " message=" << utf8(result.message) << '\n';
        anyConnected = anyConnected || static_cast<bool>(client);
    }
    return anyConnected ? 0 : 1;
}
