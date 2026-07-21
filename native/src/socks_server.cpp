#include "socks_server.h"
#include "hysteria_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace {
bool receiveAll(SOCKET socket, void* output, int length) {
    char* cursor = static_cast<char*>(output);
    while (length > 0) {
        int count = recv(socket, cursor, length, 0);
        if (count <= 0) return false;
        cursor += count; length -= count;
    }
    return true;
}

bool sendAll(SOCKET socket, const void* input, int length) {
    const char* cursor = static_cast<const char*>(input);
    while (length > 0) {
        int count = send(socket, cursor, length, 0);
        if (count <= 0) return false;
        cursor += count; length -= count;
    }
    return true;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}
}

struct SocksServer::Impl {
    SOCKET listener{INVALID_SOCKET};
    HysteriaClient* client{};
    std::atomic_bool stopping{};
    std::thread acceptThread;
    std::mutex workersMutex;
    std::vector<std::thread> workers;
    std::vector<SOCKET> clients;

    ~Impl() { stop(); }

    bool initialize(const std::wstring& address, unsigned short port, std::wstring& error) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { error = L"Не удалось запустить WinSock"; return false; }
        addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP; hints.ai_flags = AI_PASSIVE;
        addrinfo* addresses = nullptr;
        std::string host = utf8(address), service = std::to_string(port);
        int status = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
        if (status != 0) { error = L"Некорректный адрес локального SOCKS5"; WSACleanup(); return false; }
        for (auto* item = addresses; item; item = item->ai_next) {
            listener = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
            if (listener == INVALID_SOCKET) continue;
            BOOL exclusive = TRUE;
            setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
            if (bind(listener, item->ai_addr, static_cast<int>(item->ai_addrlen)) == 0 && listen(listener, SOMAXCONN) == 0) break;
            closesocket(listener); listener = INVALID_SOCKET;
        }
        freeaddrinfo(addresses);
        if (listener == INVALID_SOCKET) { error = L"Не удалось занять локальный SOCKS5-порт " + std::to_wstring(port); WSACleanup(); return false; }
        acceptThread = std::thread([this] { acceptLoop(); });
        return true;
    }

    void acceptLoop() {
        while (!stopping) {
            SOCKET socket = accept(listener, nullptr, nullptr);
            if (socket == INVALID_SOCKET) break;
            {
                std::lock_guard lock(workersMutex);
                clients.push_back(socket);
                workers.emplace_back([this, socket] { serve(socket); });
            }
        }
    }

    void serve(SOCKET socket) {
        unsigned char hello[2]{};
        if (!receiveAll(socket, hello, 2) || hello[0] != 5 || hello[1] == 0) return closeClient(socket);
        std::vector<unsigned char> methods(hello[1]);
        if (!receiveAll(socket, methods.data(), static_cast<int>(methods.size())) || std::find(methods.begin(), methods.end(), 0) == methods.end()) {
            const unsigned char reject[]{5, 0xff}; sendAll(socket, reject, sizeof(reject)); return closeClient(socket);
        }
        const unsigned char accepted[]{5, 0};
        if (!sendAll(socket, accepted, sizeof(accepted))) return closeClient(socket);
        unsigned char request[4]{};
        if (!receiveAll(socket, request, 4) || request[0] != 5 || request[1] != 1) return rejectRequest(socket, 7);
        std::string host;
        if (request[3] == 1) {
            in_addr address{}; if (!receiveAll(socket, &address, sizeof(address))) return closeClient(socket);
            char text[INET_ADDRSTRLEN]{}; if (!inet_ntop(AF_INET, &address, text, sizeof(text))) return closeClient(socket); host = text;
        } else if (request[3] == 3) {
            unsigned char length{}; if (!receiveAll(socket, &length, 1) || !length) return closeClient(socket);
            host.resize(length); if (!receiveAll(socket, host.data(), length)) return closeClient(socket);
        } else if (request[3] == 4) {
            in6_addr address{}; if (!receiveAll(socket, &address, sizeof(address))) return closeClient(socket);
            char text[INET6_ADDRSTRLEN]{}; if (!inet_ntop(AF_INET6, &address, text, sizeof(text))) return closeClient(socket); host = "[" + std::string(text) + "]";
        } else return rejectRequest(socket, 8);
        unsigned char portBytes[2]{}; if (!receiveAll(socket, portBytes, 2)) return closeClient(socket);
        unsigned port = (static_cast<unsigned>(portBytes[0]) << 8U) | portBytes[1];
        std::wstring error;
        if (!client->relayTcp(host + ":" + std::to_string(port), static_cast<std::uintptr_t>(socket), error)) { rejectRequest(socket, 5); return; }
        closeClient(socket);
    }

    void rejectRequest(SOCKET socket, unsigned char code) {
        const unsigned char reply[]{5, code, 0, 1, 0, 0, 0, 0, 0, 0};
        sendAll(socket, reply, sizeof(reply)); closeClient(socket);
    }

    void closeClient(SOCKET socket) {
        shutdown(socket, SD_BOTH); closesocket(socket);
        std::lock_guard lock(workersMutex);
        auto found = std::find(clients.begin(), clients.end(), socket);
        if (found != clients.end()) clients.erase(found);
    }

    void stop() {
        if (stopping.exchange(true)) return;
        if (listener != INVALID_SOCKET) { closesocket(listener); listener = INVALID_SOCKET; }
        if (acceptThread.joinable()) acceptThread.join();
        {
            std::lock_guard lock(workersMutex);
            for (SOCKET socket : clients) { shutdown(socket, SD_BOTH); closesocket(socket); }
            clients.clear();
        }
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        workers.clear();
        WSACleanup();
    }
};

SocksServer::SocksServer(std::unique_ptr<Impl> implementation) : implementation_(std::move(implementation)) {}
SocksServer::~SocksServer() = default;

std::unique_ptr<SocksServer> SocksServer::start(const std::wstring& address, unsigned short port, HysteriaClient& client, std::wstring& error) {
    auto implementation = std::make_unique<Impl>(); implementation->client = &client;
    if (!implementation->initialize(address, port, error)) return nullptr;
    return std::unique_ptr<SocksServer>(new SocksServer(std::move(implementation)));
}
