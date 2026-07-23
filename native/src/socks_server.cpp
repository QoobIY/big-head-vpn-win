#include "socks_server.h"
#include "tunnel_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
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

bool receiveHttpHeaders(SOCKET socket, std::string& headers) {
    std::array<char, 2048> buffer{};
    while (headers.find("\r\n\r\n") == std::string::npos && headers.size() < 16384) {
        int count = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (count <= 0) return false;
        headers.append(buffer.data(), static_cast<size_t>(count));
    }
    return headers.find("\r\n\r\n") != std::string::npos;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

bool sameIpAddress(const sockaddr_storage& left,
    const sockaddr_storage& right) {
    if (left.ss_family != right.ss_family) return false;
    if (left.ss_family == AF_INET) {
        return reinterpret_cast<const sockaddr_in*>(&left)->sin_addr.s_addr ==
            reinterpret_cast<const sockaddr_in*>(&right)->sin_addr.s_addr;
    }
    if (left.ss_family == AF_INET6) {
        return std::memcmp(
            &reinterpret_cast<const sockaddr_in6*>(&left)->sin6_addr,
            &reinterpret_cast<const sockaddr_in6*>(&right)->sin6_addr,
            sizeof(in6_addr)) == 0;
    }
    return false;
}

bool sameEndpoint(const sockaddr_storage& left,
    const sockaddr_storage& right) {
    if (!sameIpAddress(left, right)) return false;
    if (left.ss_family == AF_INET) {
        return reinterpret_cast<const sockaddr_in*>(&left)->sin_port ==
            reinterpret_cast<const sockaddr_in*>(&right)->sin_port;
    }
    return reinterpret_cast<const sockaddr_in6*>(&left)->sin6_port ==
        reinterpret_cast<const sockaddr_in6*>(&right)->sin6_port;
}

bool parseUdpPacket(const unsigned char* bytes, size_t length,
    std::string& destination, std::vector<unsigned char>& responseHeader,
    size_t& payloadOffset) {
    if (length < 7 || bytes[0] || bytes[1] || bytes[2]) return false;
    size_t cursor = 3;
    const unsigned char atyp = bytes[cursor++];
    responseHeader.clear();
    responseHeader.push_back(atyp);
    std::string host;
    if (atyp == 1) {
        if (length - cursor < 6) return false;
        in_addr address{};
        std::memcpy(&address, bytes + cursor, sizeof(address));
        char text[INET_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET, &address, text, sizeof(text))) return false;
        host = text;
        responseHeader.insert(responseHeader.end(), bytes + cursor,
            bytes + cursor + 4);
        cursor += 4;
    } else if (atyp == 3) {
        if (cursor >= length) return false;
        const size_t hostLength = bytes[cursor++];
        if (!hostLength || length - cursor < hostLength + 2) return false;
        responseHeader.push_back(static_cast<unsigned char>(hostLength));
        host.assign(reinterpret_cast<const char*>(bytes + cursor), hostLength);
        responseHeader.insert(responseHeader.end(), bytes + cursor,
            bytes + cursor + hostLength);
        cursor += hostLength;
    } else if (atyp == 4) {
        if (length - cursor < 18) return false;
        in6_addr address{};
        std::memcpy(&address, bytes + cursor, sizeof(address));
        char text[INET6_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET6, &address, text, sizeof(text))) return false;
        host = "[" + std::string(text) + "]";
        responseHeader.insert(responseHeader.end(), bytes + cursor,
            bytes + cursor + 16);
        cursor += 16;
    } else {
        return false;
    }
    if (length - cursor < 2) return false;
    const unsigned port = (static_cast<unsigned>(bytes[cursor]) << 8U) |
        bytes[cursor + 1];
    if (!port) return false;
    responseHeader.push_back(bytes[cursor]);
    responseHeader.push_back(bytes[cursor + 1]);
    cursor += 2;
    destination = host + ":" + std::to_string(port);
    payloadOffset = cursor;
    return cursor < length;
}
}

struct SocksServer::Impl {
    struct UdpAssociation {
        SOCKET socket{INVALID_SOCKET};
        std::mutex mutex;
        sockaddr_storage clientEndpoint{};
        int clientEndpointLength{};
        bool clientEndpointSet{};
        std::unordered_map<std::string, uint32_t> sessionsByDestination;
        std::vector<uint32_t> sessions;
    };
    struct UdpRoute {
        std::weak_ptr<UdpAssociation> association;
        std::vector<unsigned char> responseHeader;
    };

    SOCKET listener{INVALID_SOCKET};
    TunnelClient* client{};
    TunnelClient::UdpHandlerToken udpHandlerToken{};
    std::atomic_uint32_t nextUdpSession{2};
    std::atomic_bool stopping{};
    std::thread acceptThread;
    std::mutex workersMutex;
    std::vector<std::thread> workers;
    std::vector<SOCKET> clients;
    std::vector<SOCKET> udpSockets;
    std::mutex udpRoutesMutex;
    std::unordered_map<uint32_t, UdpRoute> udpRoutes;

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
        if (client->supportsUdp()) {
            udpHandlerToken = client->addUdpReceiveHandler(
                [this](uint32_t session, const std::string&,
                    std::vector<unsigned char> payload) {
                    deliverUdp(session, std::move(payload));
                });
        }
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
        if (!receiveAll(socket, hello, 2)) return closeClient(socket);
        if (hello[0] != 5) return serveHttpConnect(socket, hello);
        if (hello[1] == 0) return closeClient(socket);
        std::vector<unsigned char> methods(hello[1]);
        if (!receiveAll(socket, methods.data(), static_cast<int>(methods.size())) || std::find(methods.begin(), methods.end(), 0) == methods.end()) {
            const unsigned char reject[]{5, 0xff}; sendAll(socket, reject, sizeof(reject)); return closeClient(socket);
        }
        const unsigned char accepted[]{5, 0};
        if (!sendAll(socket, accepted, sizeof(accepted))) return closeClient(socket);
        unsigned char request[4]{};
        if (!receiveAll(socket, request, 4) || request[0] != 5 ||
            (request[1] != 1 && request[1] != 3))
            return rejectRequest(socket, 7);
        const unsigned char command = request[1];
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
        if (command == 3) {
            if (!client->supportsUdp()) return rejectRequest(socket, 7);
            return serveUdpAssociate(socket);
        }
        std::wstring error;
        if (!client->relayTcp(host + ":" + std::to_string(port), static_cast<std::uintptr_t>(socket), error)) {
#ifdef BIG_HEAD_VPN_TESTING
            fwprintf(stderr, L"relay_failed: %ls\n", error.c_str());
            fflush(stderr);
#endif
            rejectRequest(socket, 5); return;
        }
        closeClient(socket);
    }

    void serveUdpAssociate(SOCKET control) {
        sockaddr_storage local{}, peer{};
        int localLength = sizeof(local), peerLength = sizeof(peer);
        if (getsockname(control, reinterpret_cast<sockaddr*>(&local),
                &localLength) != 0 ||
            getpeername(control, reinterpret_cast<sockaddr*>(&peer),
                &peerLength) != 0)
            return rejectRequest(control, 1);

        auto association = std::make_shared<UdpAssociation>();
        association->socket = socket(local.ss_family, SOCK_DGRAM, IPPROTO_UDP);
        if (association->socket == INVALID_SOCKET)
            return rejectRequest(control, 1);
        if (local.ss_family == AF_INET)
            reinterpret_cast<sockaddr_in*>(&local)->sin_port = 0;
        else if (local.ss_family == AF_INET6)
            reinterpret_cast<sockaddr_in6*>(&local)->sin6_port = 0;
        else {
            closesocket(association->socket);
            return rejectRequest(control, 8);
        }
        if (bind(association->socket, reinterpret_cast<sockaddr*>(&local),
                localLength) != 0) {
            closesocket(association->socket);
            return rejectRequest(control, 1);
        }
        int boundLength = sizeof(local);
        if (getsockname(association->socket,
                reinterpret_cast<sockaddr*>(&local), &boundLength) != 0 ||
            !sendUdpAssociateReply(control, local)) {
            closesocket(association->socket);
            return closeClient(control);
        }
        {
            std::lock_guard lock(workersMutex);
            udpSockets.push_back(association->socket);
        }

        std::array<unsigned char, 65535> packet{};
        while (!stopping) {
            fd_set reads;
            FD_ZERO(&reads);
            FD_SET(control, &reads);
            FD_SET(association->socket, &reads);
            timeval timeout{1, 0};
            int selected = select(0, &reads, nullptr, nullptr, &timeout);
            if (selected == SOCKET_ERROR) break;
            if (FD_ISSET(control, &reads)) {
                unsigned char ignored{};
                if (recv(control, reinterpret_cast<char*>(&ignored), 1, 0) <= 0)
                    break;
                // RFC 1928 uses this TCP connection only as the lifetime of
                // the UDP association. Extra bytes are ignored.
            }
            if (!FD_ISSET(association->socket, &reads)) continue;
            sockaddr_storage source{};
            int sourceLength = sizeof(source);
            int received = recvfrom(association->socket,
                reinterpret_cast<char*>(packet.data()),
                static_cast<int>(packet.size()), 0,
                reinterpret_cast<sockaddr*>(&source), &sourceLength);
            if (received <= 0) continue;
            {
                std::lock_guard lock(association->mutex);
                if (!sameIpAddress(source, peer) ||
                    (association->clientEndpointSet &&
                        !sameEndpoint(source, association->clientEndpoint)))
                    continue;
                if (!association->clientEndpointSet) {
                    association->clientEndpoint = source;
                    association->clientEndpointLength = sourceLength;
                    association->clientEndpointSet = true;
                }
            }

            std::string destination;
            std::vector<unsigned char> responseHeader;
            size_t payloadOffset{};
            if (!parseUdpPacket(packet.data(), static_cast<size_t>(received),
                    destination, responseHeader, payloadOffset))
                continue;
            uint32_t session{};
            {
                std::lock_guard lock(association->mutex);
                auto [found, inserted] =
                    association->sessionsByDestination.try_emplace(
                        destination, 0);
                if (inserted) {
                    session = nextUdpSession.fetch_add(2);
                    if (!session) session = nextUdpSession.fetch_add(2);
                    found->second = session;
                    association->sessions.push_back(session);
                    std::lock_guard routesLock(udpRoutesMutex);
                    udpRoutes.emplace(session,
                        UdpRoute{association, std::move(responseHeader)});
                } else {
                    session = found->second;
                }
            }
            std::wstring error;
            client->sendUdp(session, destination,
                packet.data() + payloadOffset,
                static_cast<size_t>(received) - payloadOffset, error);
        }
        cleanupUdpAssociation(association);
        closeClient(control);
    }

    bool sendUdpAssociateReply(SOCKET control,
        const sockaddr_storage& bound) {
        std::vector<unsigned char> reply{5, 0, 0};
        if (bound.ss_family == AF_INET) {
            const auto& address =
                *reinterpret_cast<const sockaddr_in*>(&bound);
            reply.push_back(1);
            const auto* bytes =
                reinterpret_cast<const unsigned char*>(&address.sin_addr);
            reply.insert(reply.end(), bytes, bytes + sizeof(address.sin_addr));
            const auto* port =
                reinterpret_cast<const unsigned char*>(&address.sin_port);
            reply.insert(reply.end(), port, port + 2);
        } else if (bound.ss_family == AF_INET6) {
            const auto& address =
                *reinterpret_cast<const sockaddr_in6*>(&bound);
            reply.push_back(4);
            const auto* bytes =
                reinterpret_cast<const unsigned char*>(&address.sin6_addr);
            reply.insert(reply.end(), bytes, bytes + sizeof(address.sin6_addr));
            const auto* port =
                reinterpret_cast<const unsigned char*>(&address.sin6_port);
            reply.insert(reply.end(), port, port + 2);
        } else {
            return false;
        }
        return sendAll(control, reply.data(), static_cast<int>(reply.size()));
    }

    void deliverUdp(uint32_t session, std::vector<unsigned char> payload) {
        UdpRoute route;
        {
            std::lock_guard lock(udpRoutesMutex);
            auto found = udpRoutes.find(session);
            if (found == udpRoutes.end()) return;
            route = found->second;
        }
        auto association = route.association.lock();
        if (!association || stopping) return;
        std::vector<unsigned char> packet{0, 0, 0};
        packet.insert(packet.end(), route.responseHeader.begin(),
            route.responseHeader.end());
        packet.insert(packet.end(), payload.begin(), payload.end());
        std::lock_guard lock(association->mutex);
        if (!association->clientEndpointSet ||
            association->socket == INVALID_SOCKET)
            return;
        sendto(association->socket,
            reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(packet.size()), 0,
            reinterpret_cast<const sockaddr*>(
                &association->clientEndpoint),
            association->clientEndpointLength);
    }

    void cleanupUdpAssociation(
        const std::shared_ptr<UdpAssociation>& association) {
        {
            std::lock_guard routesLock(udpRoutesMutex);
            for (uint32_t session : association->sessions)
                udpRoutes.erase(session);
        }
        SOCKET udp = association->socket;
        bool closeSocket{};
        {
            std::lock_guard lock(association->mutex);
            association->socket = INVALID_SOCKET;
        }
        {
            std::lock_guard lock(workersMutex);
            auto found = std::find(udpSockets.begin(), udpSockets.end(), udp);
            if (found != udpSockets.end()) {
                udpSockets.erase(found);
                closeSocket = true;
            }
        }
        if (closeSocket && udp != INVALID_SOCKET) closesocket(udp);
    }

    void serveHttpConnect(SOCKET socket, const unsigned char first[2]) {
        std::string headers(reinterpret_cast<const char*>(first), 2);
        if (!receiveHttpHeaders(socket, headers)) return closeClient(socket);
        size_t lineEnd = headers.find("\r\n");
        std::string line = headers.substr(0, lineEnd);
        size_t firstSpace = line.find(' ');
        size_t secondSpace = firstSpace == std::string::npos ? firstSpace : line.find(' ', firstSpace + 1);
        std::string method = firstSpace == std::string::npos ? line : line.substr(0, firstSpace);
        std::transform(method.begin(), method.end(), method.begin(), [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
        if (method != "CONNECT" || firstSpace == std::string::npos || secondSpace == std::string::npos || secondSpace == firstSpace + 1) {
            static constexpr char rejected[] = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(socket, rejected, static_cast<int>(sizeof(rejected) - 1));
            return closeClient(socket);
        }
        std::string destination = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        static constexpr char established[] = "HTTP/1.1 200 Connection Established\r\nProxy-Agent: BigHeadVPN\r\n\r\n";
        if (!sendAll(socket, established, static_cast<int>(sizeof(established) - 1))) return closeClient(socket);
        std::wstring error;
        client->relayTcp(destination, static_cast<std::uintptr_t>(socket), error, false);
        closeClient(socket);
    }

    void rejectRequest(SOCKET socket, unsigned char code) {
        const unsigned char reply[]{5, code, 0, 1, 0, 0, 0, 0, 0, 0};
        sendAll(socket, reply, sizeof(reply)); closeClient(socket);
    }

    void closeClient(SOCKET socket) {
        bool owned{};
        {
            std::lock_guard lock(workersMutex);
            auto found = std::find(clients.begin(), clients.end(), socket);
            if (found != clients.end()) {
                clients.erase(found);
                owned = true;
            }
        }
        // stop() may already have closed and removed this socket. Avoid
        // closing a recycled WinSock handle from the worker during shutdown.
        if (owned) {
            shutdown(socket, SD_BOTH);
            closesocket(socket);
        }
    }

    void stop() {
        if (stopping.exchange(true)) return;
        if (client && udpHandlerToken) {
            client->removeUdpReceiveHandler(udpHandlerToken);
            udpHandlerToken = 0;
        }
        if (listener != INVALID_SOCKET) { closesocket(listener); listener = INVALID_SOCKET; }
        if (acceptThread.joinable()) acceptThread.join();
        {
            std::lock_guard lock(workersMutex);
            for (SOCKET socket : clients) { shutdown(socket, SD_BOTH); closesocket(socket); }
            for (SOCKET socket : udpSockets) closesocket(socket);
            clients.clear();
            udpSockets.clear();
        }
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        workers.clear();
        WSACleanup();
    }
};

SocksServer::SocksServer(std::unique_ptr<Impl> implementation) : implementation_(std::move(implementation)) {}
SocksServer::~SocksServer() = default;

std::unique_ptr<SocksServer> SocksServer::start(const std::wstring& address, unsigned short port, TunnelClient& client, std::wstring& error) {
    auto implementation = std::make_unique<Impl>(); implementation->client = &client;
    if (!implementation->initialize(address, port, error)) return nullptr;
    return std::unique_ptr<SocksServer>(new SocksServer(std::move(implementation)));
}
