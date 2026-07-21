#include "hysteria_client.h"
#include "socks_server.h"

#include <windows.h>

#include <chrono>
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
    if (argc == 2 && std::wstring_view(argv[1]) == L"--fixture") {
        for (unsigned char byte : hysteriaAuthFixtureForTest())
            std::cout << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
        std::cout << '\n';
        return 0;
    }
    bool socksMode = argc == 4 && std::wstring_view(argv[1]) == L"--socks";
    if (argc != 2 && !socksMode) { std::wcerr << L"usage: BigHeadVPNProbe subscription-file\n"; return 2; }
    std::ifstream input(std::filesystem::path(argv[socksMode ? 2 : 1]), std::ios::binary);
    std::string line;
    std::vector<std::string> profiles;
    while (std::getline(input, line)) {
        if (line.rfind("hysteria2://", 0) == 0 || line.rfind("hy2://", 0) == 0) profiles.push_back(line);
    }
    if (profiles.empty()) { std::wcerr << L"no Hysteria2 profile\n"; return 3; }
    if (socksMode) {
        HysteriaConnectResult result;
        auto client = HysteriaClient::connect(wide(profiles.front()), result);
        if (!client) { std::cout << "connect_failed\n"; return 1; }
        wchar_t* end{}; unsigned long port = wcstoul(argv[3], &end, 10);
        std::wstring error;
        auto server = SocksServer::start(L"127.0.0.1", static_cast<unsigned short>(port), *client, error);
        if (!server) { std::cout << "listen_failed\n"; return 1; }
        std::cout << "socks_ready\n" << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(20));
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
