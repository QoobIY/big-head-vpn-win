#include "process_filter.h"
#include "tunnel_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <windivert.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
struct FlowKey {
    uint32_t remoteAddress{};
    uint16_t localPort{};
    uint16_t remotePort{};
    bool operator==(const FlowKey&) const = default;
};
struct FlowHash {
    size_t operator()(const FlowKey& key) const noexcept {
        return key.remoteAddress ^ (static_cast<size_t>(key.localPort) << 32U) ^ (static_cast<size_t>(key.remotePort) << 48U);
    }
};
struct Flow6Key {
    std::array<uint32_t, 4> remoteAddress{};
    uint16_t localPort{}, remotePort{};
    bool operator==(const Flow6Key&) const = default;
};
struct Flow6Hash {
    size_t operator()(const Flow6Key& key) const noexcept {
        size_t value = key.localPort ^ (static_cast<size_t>(key.remotePort) << 16U);
        for (auto part : key.remoteAddress) value = (value * 16777619U) ^ part;
        return value;
    }
};
struct NatKey {
    uint32_t remoteAddress{};
    uint16_t clientPort{};
    bool operator==(const NatKey&) const = default;
};
struct NatHash {
    size_t operator()(const NatKey& key) const noexcept { return key.remoteAddress ^ (static_cast<size_t>(key.clientPort) << 32U); }
};
struct FlowState {
    bool selected{};
    UINT64 endpoint{};
};
struct NatState {
    uint16_t remotePort{};
    UINT64 endpoint{};
    uint32_t remoteAddress{};
    uint32_t localAddress{};
    DWORD interfaceIndex{};
};
struct UdpState {
    uint32_t session{};
    uint32_t remoteAddress{}, localAddress{};
    uint16_t remotePort{}, localPort{};
    DWORD interfaceIndex{};
};
struct Nat6State {
    uint16_t remotePort{};
    UINT64 endpoint{};
    std::array<uint32_t, 4> remoteAddress{}, localAddress{};
    DWORD interfaceIndex{};
};
struct Udp6State {
    uint32_t session{};
    std::array<uint32_t, 4> remoteAddress{}, localAddress{};
    uint16_t remotePort{}, localPort{};
    DWORD interfaceIndex{};
};

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

std::wstring processName(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[32768]{}; DWORD length = static_cast<DWORD>(std::size(path));
    bool ok = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    return ok ? lower(std::filesystem::path(std::wstring(path, length)).filename().wstring()) : std::wstring{};
}

bool isNetworkPath(const std::wstring& path) {
    return path.rfind(LR"(\\)", 0) == 0 || path.rfind(LR"(\\?\UNC\)", 0) == 0;
}

std::wstring win32Message(DWORD code) {
    wchar_t* buffer{};
    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    if (!size || !buffer) return {};
    std::wstring message(buffer, size);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L'.' || message.back() == L' ')) message.pop_back();
    return message;
}

bool transientWinDivertDriverError(DWORD code) {
    return code == ERROR_BAD_NET_NAME || code == ERROR_PATH_NOT_FOUND || code == ERROR_FILE_NOT_FOUND || code == ERROR_SERVICE_EXISTS;
}

bool stopAndDeleteService(SC_HANDLE manager, const wchar_t* name) {
    SC_HANDLE service = OpenServiceW(manager, name, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!service) return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST;

    SERVICE_STATUS status{};
    ControlService(service, SERVICE_CONTROL_STOP, &status);
    for (int attempt = 0; attempt < 20; ++attempt) {
        SERVICE_STATUS_PROCESS processStatus{};
        DWORD needed{};
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&processStatus), sizeof(processStatus), &needed)) break;
        if (processStatus.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(100);
    }

    BOOL deleted = DeleteService(service);
    DWORD errorCode = GetLastError();
    CloseServiceHandle(service);
    return deleted || errorCode == ERROR_SERVICE_MARKED_FOR_DELETE || errorCode == ERROR_SERVICE_DOES_NOT_EXIST;
}

bool resetWinDivertService() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return false;
    bool ok = stopAndDeleteService(manager, L"WinDivert");
    ok = stopAndDeleteService(manager, L"WinDivert1.4") && ok;
    ok = stopAndDeleteService(manager, L"WinDivert2") && ok;
    CloseServiceHandle(manager);
    return ok;
}

std::wstring windivertOpenError(const wchar_t* layer, DWORD code, const std::wstring& executablePath) {
    if (code == ERROR_ACCESS_DENIED) return L"Фильтру процессов нужны права администратора";
    if (code == ERROR_BAD_NET_NAME || isNetworkPath(executablePath)) {
        return std::wstring(L"WinDivert ") + layer +
            L" не может загрузить драйвер. Попробуйте запустить EXE с локального пути без OneDrive/синхронизации, например C:\\BigHeadVPN-Native.";
    }
    std::wstring details = win32Message(code);
    return std::wstring(L"WinDivert ") + layer + L" error " + std::to_wstring(code) + (details.empty() ? L"" : L": " + details);
}

std::filesystem::path findWinDivertRuntime(const std::filesystem::path& exeDir, std::wstring& error) {
    auto sourceDll = exeDir / L"WinDivert.dll";
    auto sourceDriver = exeDir / L"WinDivert64.sys";
    std::error_code fsError;
    if (!std::filesystem::exists(sourceDll, fsError)) {
        error = L"Рядом с EXE отсутствует WinDivert.dll";
        return {};
    }
    if (!std::filesystem::exists(sourceDriver, fsError)) {
        error = L"Рядом с EXE отсутствует WinDivert64.sys";
        return {};
    }

    return sourceDll;
}
}

struct ProcessFilter::Impl {
    using OpenFn = decltype(&WinDivertOpen);
    using RecvFn = decltype(&WinDivertRecv);
    using SendFn = decltype(&WinDivertSend);
    using CloseFn = decltype(&WinDivertClose);
    using ParseFn = decltype(&WinDivertHelperParsePacket);
    using ChecksumFn = decltype(&WinDivertHelperCalcChecksums);
    using Format6Fn = decltype(&WinDivertHelperFormatIPv6Address);

    HMODULE module{};
    OpenFn open{}; RecvFn receive{}; SendFn inject{}; CloseFn close{};
    ParseFn parse{}; ChecksumFn checksums{}; Format6Fn format6{};
    HANDLE socketHandle{INVALID_HANDLE_VALUE}, flowHandle{INVALID_HANDLE_VALUE}, networkHandle{INVALID_HANDLE_VALUE};
    SOCKET listener{INVALID_SOCKET}, listener6{INVALID_SOCKET};
    uint16_t proxyPort{};
    TunnelClient* client{};
    std::vector<std::wstring> names;
    std::atomic_bool stopping{};
    std::atomic_ullong matched{}, redirected{}, proxyReplies{}, accepted{}, acceptAttempts{}, natMisses{}, udpMatched{}, udpSent{}, udpReceived{}, socketEventCount{}, namedProcessCount{}, injectionFailureCount{};
    std::atomic_ulong injectionError{};
    std::atomic_uint32_t nextUdpSession{1};
    std::thread socketThread, flowThread, networkThread, acceptThread, acceptThread6;
    std::mutex mutex, workersMutex;
    std::unordered_map<FlowKey, FlowState, FlowHash> selectedFlows;
    std::unordered_map<NatKey, NatState, NatHash> nat;
    std::unordered_map<Flow6Key, FlowState, Flow6Hash> selectedFlows6;
    std::unordered_map<Flow6Key, Nat6State, Flow6Hash> nat6;
    std::unordered_map<FlowKey, FlowState, FlowHash> selectedUdpFlows;
    std::unordered_map<Flow6Key, FlowState, Flow6Hash> selectedUdpFlows6;
    std::unordered_map<FlowKey, UdpState, FlowHash> udpSessions;
    std::unordered_map<uint32_t, UdpState> udpBySession;
    std::unordered_map<Flow6Key, Udp6State, Flow6Hash> udpSessions6;
    std::unordered_map<uint32_t, Udp6State> udp6BySession;
    std::vector<std::thread> workers;
    std::vector<SOCKET> clients;

    ~Impl() { stop(); }

    bool initialize(std::wstring& error) {
        wchar_t executable[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) { error = L"Не найдена папка приложения"; return false; }
        auto dll = findWinDivertRuntime(std::filesystem::path(executable).parent_path(), error);
        if (dll.empty()) return false;
        module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) { error = L"Не удалось загрузить WinDivert.dll из " + dll.parent_path().wstring() + L": " + win32Message(GetLastError()); return false; }
#define LOAD(name, field) field = reinterpret_cast<decltype(field)>(GetProcAddress(module, name)); if (!field) { error = L"Некорректная WinDivert.dll"; return false; }
        LOAD("WinDivertOpen", open) LOAD("WinDivertRecv", receive) LOAD("WinDivertSend", inject)
        LOAD("WinDivertClose", close) LOAD("WinDivertHelperParsePacket", parse)
        LOAD("WinDivertHelperCalcChecksums", checksums) LOAD("WinDivertHelperFormatIPv6Address", format6)
#undef LOAD
        WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { error = L"Не удалось запустить WinSock"; return false; }
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK); local.sin_port = 0;
        if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 || listen(listener, SOMAXCONN) != 0) {
            error = L"Не удалось открыть transparent TCP relay"; return false;
        }
        int localLength = sizeof(local); getsockname(listener, reinterpret_cast<sockaddr*>(&local), &localLength); proxyPort = ntohs(local.sin_port);
        listener6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (listener6 != INVALID_SOCKET) {
            int v6Only = 1; setsockopt(listener6, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6Only), sizeof(v6Only));
            sockaddr_in6 local6{}; local6.sin6_family = AF_INET6; local6.sin6_addr = in6addr_loopback; local6.sin6_port = htons(proxyPort);
            if (bind(listener6, reinterpret_cast<sockaddr*>(&local6), sizeof(local6)) != 0 || listen(listener6, SOMAXCONN) != 0) {
                closesocket(listener6); listener6 = INVALID_SOCKET;
            }
        }
        std::wstring executablePath(executable);
        socketHandle = open("true", WINDIVERT_LAYER_SOCKET, 1200, WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
        if (socketHandle == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            if (transientWinDivertDriverError(code) && resetWinDivertService()) {
                Sleep(300);
                socketHandle = open("true", WINDIVERT_LAYER_SOCKET, 1200, WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
                if (socketHandle == INVALID_HANDLE_VALUE) code = GetLastError();
            }
            if (socketHandle == INVALID_HANDLE_VALUE) { error = windivertOpenError(L"SOCKET", code, executablePath); return false; }
        }
        if (client->supportsUdp()) {
            flowHandle = open("outbound and udp", WINDIVERT_LAYER_FLOW, 1200, WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
            if (flowHandle == INVALID_HANDLE_VALUE) { error = windivertOpenError(L"FLOW", GetLastError(), executablePath); return false; }
        }
        std::string networkFilter = "tcp and outbound and (!loopback or tcp.SrcPort == " + std::to_string(proxyPort) + ")";
        if (client->supportsUdp()) networkFilter = "(" + networkFilter + ") or (udp and outbound and !loopback)";
        networkHandle = open(networkFilter.c_str(), WINDIVERT_LAYER_NETWORK, 1100, 0);
        if (networkHandle == INVALID_HANDLE_VALUE) { error = windivertOpenError(L"NETWORK", GetLastError(), executablePath); return false; }
        nextUdpSession = static_cast<uint32_t>(GetTickCount64()) | 1U;
        if (client->supportsUdp()) client->setUdpReceiveHandler([this](uint32_t session, const std::string&, std::vector<unsigned char> payload) {
            injectUdp(session, std::move(payload));
        });
        socketThread = std::thread([this] { socketLoop(); });
        if (client->supportsUdp()) flowThread = std::thread([this] { udpFlowLoop(); });
        networkThread = std::thread([this] { networkLoop(); });
        acceptThread = std::thread([this] { acceptLoop(); });
        if (listener6 != INVALID_SOCKET) acceptThread6 = std::thread([this] { acceptLoop6(); });
        return true;
    }

    bool selected(DWORD pid) {
        if (pid == GetCurrentProcessId()) return false;
        auto name = processName(pid);
        if (!name.empty()) ++namedProcessCount;
        return !name.empty() && std::find(names.begin(), names.end(), name) != names.end();
    }

    bool ipv4FromSocket(const UINT32 address[4], uint32_t& result) {
        char text[64]{}; if (!format6(address, text, sizeof(text))) return false;
        const char* value = std::strrchr(text, ':'); value = value ? value + 1 : text;
        in_addr parsed{}; if (inet_pton(AF_INET, value, &parsed) != 1) return false;
        result = parsed.s_addr; return true;
    }

    static std::array<uint32_t, 4> address6(const UINT32 address[4]) {
        return {address[0], address[1], address[2], address[3]};
    }

    void socketLoop() {
        WINDIVERT_ADDRESS address{};
        while (!stopping && receive(socketHandle, nullptr, 0, nullptr, &address)) {
            ++socketEventCount;
            if (address.Socket.Protocol != IPPROTO_TCP || (address.Event != WINDIVERT_EVENT_SOCKET_CONNECT && address.Event != WINDIVERT_EVENT_SOCKET_CLOSE)) continue;
            uint32_t remote{};
            if (!ipv4FromSocket(address.Socket.RemoteAddr, remote)) {
                Flow6Key key{address6(address.Socket.RemoteAddr), address.Socket.LocalPort, address.Socket.RemotePort};
                std::lock_guard lock(mutex);
                if (address.Event == WINDIVERT_EVENT_SOCKET_CONNECT) {
                    bool matches = selected(address.Socket.ProcessId); selectedFlows6[key] = {matches, address.Socket.EndpointId}; if (matches) ++matched;
                } else {
                    auto flow = selectedFlows6.find(key);
                    if (flow != selectedFlows6.end() && flow->second.endpoint == address.Socket.EndpointId) { selectedFlows6.erase(flow); nat6.erase(key); }
                }
                continue;
            }
            FlowKey key{remote, address.Socket.LocalPort, address.Socket.RemotePort};
            std::lock_guard lock(mutex);
            if (address.Event == WINDIVERT_EVENT_SOCKET_CONNECT) {
                bool matches = selected(address.Socket.ProcessId); selectedFlows[key] = {matches, address.Socket.EndpointId}; if (matches) ++matched;
            }
            else if (address.Event == WINDIVERT_EVENT_SOCKET_CLOSE) {
                auto flow = selectedFlows.find(key);
                if (flow != selectedFlows.end() && flow->second.endpoint == address.Socket.EndpointId) {
                    selectedFlows.erase(flow);
                    auto mapping = nat.find({remote, address.Socket.LocalPort});
                    if (mapping != nat.end() && mapping->second.endpoint == address.Socket.EndpointId) nat.erase(mapping);
                }
            }
        }
    }

    void udpFlowLoop() {
        WINDIVERT_ADDRESS address{};
        while (!stopping && receive(flowHandle, nullptr, 0, nullptr, &address)) {
            if (address.Flow.Protocol != IPPROTO_UDP) continue;
            uint32_t remote{};
            if (!ipv4FromSocket(address.Flow.RemoteAddr, remote)) {
                Flow6Key key{address6(address.Flow.RemoteAddr), address.Flow.LocalPort, address.Flow.RemotePort};
                std::lock_guard lock(mutex);
                if (address.Event == WINDIVERT_EVENT_FLOW_ESTABLISHED) {
                    bool matches = selected(address.Flow.ProcessId); selectedUdpFlows6[key] = {matches, address.Flow.EndpointId}; if (matches) ++udpMatched;
                } else if (address.Event == WINDIVERT_EVENT_FLOW_DELETED) {
                    auto flow = selectedUdpFlows6.find(key);
                    if (flow != selectedUdpFlows6.end() && flow->second.endpoint == address.Flow.EndpointId) {
                        selectedUdpFlows6.erase(flow); auto session = udpSessions6.find(key);
                        if (session != udpSessions6.end()) { udp6BySession.erase(session->second.session); udpSessions6.erase(session); }
                    }
                }
                continue;
            }
            FlowKey key{remote, address.Flow.LocalPort, address.Flow.RemotePort};
            std::lock_guard lock(mutex);
            if (address.Event == WINDIVERT_EVENT_FLOW_ESTABLISHED) {
                bool matches = selected(address.Flow.ProcessId); selectedUdpFlows[key] = {matches, address.Flow.EndpointId}; if (matches) ++udpMatched;
            } else if (address.Event == WINDIVERT_EVENT_FLOW_DELETED) {
                auto flow = selectedUdpFlows.find(key);
                if (flow != selectedUdpFlows.end() && flow->second.endpoint == address.Flow.EndpointId) {
                    selectedUdpFlows.erase(flow);
                    auto session = udpSessions.find(key);
                    if (session != udpSessions.end()) { udpBySession.erase(session->second.session); udpSessions.erase(session); }
                }
            }
        }
    }

    void networkLoop() {
        std::vector<unsigned char> packet(WINDIVERT_MTU_MAX);
        WINDIVERT_ADDRESS address{}; UINT length{};
        while (!stopping && receive(networkHandle, packet.data(), static_cast<UINT>(packet.size()), &length, &address)) {
            PWINDIVERT_IPHDR ip{}; PWINDIVERT_IPV6HDR ip6{}; PWINDIVERT_TCPHDR tcp{}; PWINDIVERT_UDPHDR udp{}; void* payload{}; UINT payloadLength{};
            parse(packet.data(), length, &ip, &ip6, nullptr, nullptr, nullptr, &tcp, &udp, &payload, &payloadLength, nullptr, nullptr);
            if (ip6 && udp) {
                Flow6Key key{address6(ip6->DstAddr), ntohs(udp->SrcPort), ntohs(udp->DstPort)};
                Udp6State state{}; bool divert = false;
                {
                    std::lock_guard lock(mutex); auto flow = selectedUdpFlows6.find(key); divert = flow != selectedUdpFlows6.end() && flow->second.selected;
                    if (divert) {
                        auto found = udpSessions6.find(key);
                        if (found == udpSessions6.end()) {
                            state.session = nextUdpSession.fetch_add(2); state.remoteAddress = address6(ip6->DstAddr); state.localAddress = address6(ip6->SrcAddr);
                            state.remotePort = key.remotePort; state.localPort = key.localPort; state.interfaceIndex = address.Network.IfIdx;
                            if (!state.interfaceIndex) { sockaddr_in6 destination{}; destination.sin6_family = AF_INET6; std::memcpy(&destination.sin6_addr, ip6->DstAddr, 16); GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&destination), &state.interfaceIndex); }
                            udpSessions6.emplace(key, state); udp6BySession.emplace(state.session, state);
                        } else state = found->second;
                    }
                }
                if (divert) {
                    char host[INET6_ADDRSTRLEN]{}; inet_ntop(AF_INET6, state.remoteAddress.data(), host, sizeof(host)); std::wstring udpError;
                    if (client->sendUdp(state.session, "[" + std::string(host) + "]:" + std::to_string(state.remotePort),
                        static_cast<const unsigned char*>(payload), payloadLength, udpError)) ++udpSent;
                    continue;
                }
                inject(networkHandle, packet.data(), length, nullptr, &address); continue;
            }
            if (ip && udp) {
                FlowKey key{ip->DstAddr, ntohs(udp->SrcPort), ntohs(udp->DstPort)};
                UdpState state{}; bool divert = false;
                {
                    std::lock_guard lock(mutex);
                    auto flow = selectedUdpFlows.find(key); divert = flow != selectedUdpFlows.end() && flow->second.selected;
                    if (divert) {
                        auto found = udpSessions.find(key);
                        if (found == udpSessions.end()) {
                            DWORD interfaceIndex = address.Network.IfIdx;
                            if (!interfaceIndex) { sockaddr_in destination{}; destination.sin_family = AF_INET; destination.sin_addr.s_addr = ip->DstAddr; GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&destination), &interfaceIndex); }
                            state = {nextUdpSession.fetch_add(2), ip->DstAddr, ip->SrcAddr, key.remotePort, key.localPort, interfaceIndex};
                            udpSessions.emplace(key, state); udpBySession.emplace(state.session, state);
                        } else state = found->second;
                    }
                }
                if (divert) {
                    char host[INET_ADDRSTRLEN]{}; in_addr remote{}; remote.s_addr = state.remoteAddress; inet_ntop(AF_INET, &remote, host, sizeof(host));
                    std::wstring udpError;
                    if (client->sendUdp(state.session, std::string(host) + ":" + std::to_string(state.remotePort),
                        static_cast<const unsigned char*>(payload), payloadLength, udpError)) ++udpSent;
                    continue;
                }
                inject(networkHandle, packet.data(), length, nullptr, &address); continue;
            }
            if (ip6 && tcp) {
                if (ntohs(tcp->SrcPort) == proxyPort) {
                    ++proxyReplies; Nat6State state{};
                    { std::lock_guard lock(mutex); for (const auto& [key, value] : nat6) if (key.localPort == ntohs(tcp->DstPort)) { state = value; break; } }
                    if (state.remotePort) {
                        tcp->SrcPort = htons(state.remotePort);
                        std::copy(state.remoteAddress.begin(), state.remoteAddress.end(), ip6->SrcAddr);
                        std::copy(state.localAddress.begin(), state.localAddress.end(), ip6->DstAddr);
                        address.Outbound = FALSE; address.Loopback = FALSE; address.Network.IfIdx = state.interfaceIndex; address.Network.SubIfIdx = 0;
                    }
                } else if (!address.Loopback) {
                    Flow6Key key{address6(ip6->DstAddr), ntohs(tcp->SrcPort), ntohs(tcp->DstPort)};
                    bool divert = false; UINT64 endpoint = 0;
                    { std::lock_guard lock(mutex); auto found = selectedFlows6.find(key); if (found != selectedFlows6.end()) { divert = found->second.selected; endpoint = found->second.endpoint; } }
                    if (divert) {
                        ++redirected; Nat6State state{}; state.remotePort = key.remotePort; state.endpoint = endpoint;
                        state.remoteAddress = address6(ip6->DstAddr); state.localAddress = address6(ip6->SrcAddr); state.interfaceIndex = address.Network.IfIdx;
                        if (!state.interfaceIndex) { sockaddr_in6 destination{}; destination.sin6_family = AF_INET6; std::memcpy(&destination.sin6_addr, ip6->DstAddr, 16); GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&destination), &state.interfaceIndex); }
                        { std::lock_guard lock(mutex); nat6[key] = state; }
                        tcp->DstPort = htons(proxyPort); std::memset(ip6->SrcAddr, 0, 16); std::memset(ip6->DstAddr, 0, 16);
                        reinterpret_cast<unsigned char*>(ip6->SrcAddr)[15] = 1; reinterpret_cast<unsigned char*>(ip6->DstAddr)[15] = 1;
                        address.Outbound = TRUE; address.Loopback = TRUE;
                    }
                }
                checksums(packet.data(), length, &address, 0);
                if (!inject(networkHandle, packet.data(), length, nullptr, &address)) { ++injectionFailureCount; injectionError = GetLastError(); }
                continue;
            }
            if (!ip || !tcp) { inject(networkHandle, packet.data(), length, nullptr, &address); continue; }
            if (ntohs(tcp->SrcPort) == proxyPort) {
                ++proxyReplies;
                NatState state{};
                {
                    std::lock_guard lock(mutex);
                    for (const auto& [key, value] : nat) if (key.clientPort == ntohs(tcp->DstPort)) { state = value; break; }
                }
                if (state.remotePort) {
                    tcp->SrcPort = htons(state.remotePort);
                    ip->SrcAddr = state.remoteAddress;
                    ip->DstAddr = state.localAddress;
                    address.Outbound = FALSE; address.Loopback = FALSE;
                    address.Network.IfIdx = state.interfaceIndex;
                    address.Network.SubIfIdx = 0;
                }
            } else if (!address.Loopback) {
                FlowKey key{ip->DstAddr, ntohs(tcp->SrcPort), ntohs(tcp->DstPort)};
                bool divert = false; UINT64 endpoint = 0;
                { std::lock_guard lock(mutex); auto found = selectedFlows.find(key); if (found != selectedFlows.end()) { divert = found->second.selected; endpoint = found->second.endpoint; } }
                if (divert) {
                    ++redirected;
                    uint32_t remoteAddress = ip->DstAddr, localAddress = ip->SrcAddr;
                    DWORD interfaceIndex = address.Network.IfIdx;
                    if (!interfaceIndex) {
                        sockaddr_in destination{}; destination.sin_family = AF_INET; destination.sin_addr.s_addr = remoteAddress;
                        GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&destination), &interfaceIndex);
                    }
                    { std::lock_guard lock(mutex); nat[{remoteAddress, key.localPort}] = {key.remotePort, endpoint, remoteAddress, localAddress, interfaceIndex}; }
                    tcp->DstPort = htons(proxyPort);
                    ip->SrcAddr = htonl(INADDR_LOOPBACK);
                    ip->DstAddr = htonl(INADDR_LOOPBACK);
                    address.Outbound = TRUE; address.Loopback = TRUE;
                }
            }
            checksums(packet.data(), length, &address, 0);
            if (!inject(networkHandle, packet.data(), length, nullptr, &address)) { ++injectionFailureCount; injectionError = GetLastError(); }
        }
    }

    void injectUdp(uint32_t session, std::vector<unsigned char> payload) {
        if (stopping || payload.size() > 65507) return;
        UdpState state{};
        Udp6State state6{}; bool ipv6 = false;
        { std::lock_guard lock(mutex); auto found = udpBySession.find(session); if (found != udpBySession.end()) state = found->second;
          else { auto found6 = udp6BySession.find(session); if (found6 == udp6BySession.end()) return; state6 = found6->second; ipv6 = true; } }
        if (ipv6) { injectUdp6(state6, std::move(payload)); return; }
        std::vector<unsigned char> packet(sizeof(WINDIVERT_IPHDR) + sizeof(WINDIVERT_UDPHDR) + payload.size());
        auto* ip = reinterpret_cast<PWINDIVERT_IPHDR>(packet.data());
        auto* udp = reinterpret_cast<PWINDIVERT_UDPHDR>(packet.data() + sizeof(WINDIVERT_IPHDR));
        ip->Version = 4; ip->HdrLength = 5; ip->Length = htons(static_cast<uint16_t>(packet.size())); ip->TTL = 64; ip->Protocol = IPPROTO_UDP;
        ip->SrcAddr = state.remoteAddress; ip->DstAddr = state.localAddress;
        udp->SrcPort = htons(state.remotePort); udp->DstPort = htons(state.localPort); udp->Length = htons(static_cast<uint16_t>(sizeof(WINDIVERT_UDPHDR) + payload.size()));
        std::copy(payload.begin(), payload.end(), packet.begin() + static_cast<std::ptrdiff_t>(sizeof(WINDIVERT_IPHDR) + sizeof(WINDIVERT_UDPHDR)));
        WINDIVERT_ADDRESS address{}; address.Layer = WINDIVERT_LAYER_NETWORK; address.Event = WINDIVERT_EVENT_NETWORK_PACKET;
        address.Outbound = FALSE; address.Network.IfIdx = state.interfaceIndex;
        checksums(packet.data(), static_cast<UINT>(packet.size()), &address, 0);
        if (inject(networkHandle, packet.data(), static_cast<UINT>(packet.size()), nullptr, &address)) ++udpReceived;
        else { ++injectionFailureCount; injectionError = GetLastError(); }
    }

    void injectUdp6(const Udp6State& state, std::vector<unsigned char> payload) {
        if (payload.size() > 65527) return;
        std::vector<unsigned char> packet(sizeof(WINDIVERT_IPV6HDR) + sizeof(WINDIVERT_UDPHDR) + payload.size());
        auto* ip = reinterpret_cast<PWINDIVERT_IPV6HDR>(packet.data());
        auto* udp = reinterpret_cast<PWINDIVERT_UDPHDR>(packet.data() + sizeof(WINDIVERT_IPV6HDR));
        ip->Version = 6; ip->Length = htons(static_cast<uint16_t>(sizeof(WINDIVERT_UDPHDR) + payload.size())); ip->NextHdr = IPPROTO_UDP; ip->HopLimit = 64;
        std::copy(state.remoteAddress.begin(), state.remoteAddress.end(), ip->SrcAddr); std::copy(state.localAddress.begin(), state.localAddress.end(), ip->DstAddr);
        udp->SrcPort = htons(state.remotePort); udp->DstPort = htons(state.localPort); udp->Length = htons(static_cast<uint16_t>(sizeof(WINDIVERT_UDPHDR) + payload.size()));
        std::copy(payload.begin(), payload.end(), packet.begin() + static_cast<std::ptrdiff_t>(sizeof(WINDIVERT_IPV6HDR) + sizeof(WINDIVERT_UDPHDR)));
        WINDIVERT_ADDRESS address{}; address.Layer = WINDIVERT_LAYER_NETWORK; address.Event = WINDIVERT_EVENT_NETWORK_PACKET; address.IPv6 = TRUE;
        address.Outbound = FALSE; address.Network.IfIdx = state.interfaceIndex; checksums(packet.data(), static_cast<UINT>(packet.size()), &address, 0);
        if (inject(networkHandle, packet.data(), static_cast<UINT>(packet.size()), nullptr, &address)) ++udpReceived;
        else { ++injectionFailureCount; injectionError = GetLastError(); }
    }

    void acceptLoop() {
        while (!stopping) {
            sockaddr_in peer{}; int peerLength = sizeof(peer); SOCKET socket = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (socket == INVALID_SOCKET) break;
            ++acceptAttempts;
            uint16_t destinationPort{};
            in_addr destinationAddress{};
            {
                std::lock_guard lock(mutex);
                auto found = nat.find({peer.sin_addr.s_addr, ntohs(peer.sin_port)});
                if (found != nat.end()) { destinationPort = found->second.remotePort; destinationAddress.s_addr = found->first.remoteAddress; }
                else for (const auto& [key, state] : nat) if (key.clientPort == ntohs(peer.sin_port)) { destinationPort = state.remotePort; destinationAddress.s_addr = key.remoteAddress; break; }
            }
            if (!destinationPort) { ++natMisses; closesocket(socket); continue; }
            char host[INET_ADDRSTRLEN]{}; inet_ntop(AF_INET, &destinationAddress, host, sizeof(host));
            ++accepted; std::string destination = std::string(host) + ":" + std::to_string(destinationPort);
            std::lock_guard lock(workersMutex); clients.push_back(socket); workers.emplace_back([this, socket, destination = std::move(destination)] { relay(socket, destination); });
        }
    }

    void acceptLoop6() {
        while (!stopping) {
            sockaddr_in6 peer{}; int peerLength = sizeof(peer); SOCKET socket = accept(listener6, reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (socket == INVALID_SOCKET) break;
            ++acceptAttempts; Nat6State state{};
            { std::lock_guard lock(mutex); for (const auto& [key, value] : nat6) if (key.localPort == ntohs(peer.sin6_port)) { state = value; break; } }
            if (!state.remotePort) { ++natMisses; closesocket(socket); continue; }
            char host[INET6_ADDRSTRLEN]{}; inet_ntop(AF_INET6, state.remoteAddress.data(), host, sizeof(host));
            ++accepted; std::string destination = "[" + std::string(host) + "]:" + std::to_string(state.remotePort);
            std::lock_guard lock(workersMutex); clients.push_back(socket); workers.emplace_back([this, socket, destination = std::move(destination)] { relay(socket, destination); });
        }
    }

    void relay(SOCKET socket, const std::string& destination) {
        std::wstring error; client->relayTcp(destination, static_cast<std::uintptr_t>(socket), error, false);
        shutdown(socket, SD_BOTH); closesocket(socket);
        std::lock_guard lock(workersMutex); auto found = std::find(clients.begin(), clients.end(), socket); if (found != clients.end()) clients.erase(found);
    }

    void stop() {
        if (stopping.exchange(true)) return;
        if (client) client->setUdpReceiveHandler({});
        if (socketHandle != INVALID_HANDLE_VALUE && close) { close(socketHandle); socketHandle = INVALID_HANDLE_VALUE; }
        if (flowHandle != INVALID_HANDLE_VALUE && close) { close(flowHandle); flowHandle = INVALID_HANDLE_VALUE; }
        if (networkHandle != INVALID_HANDLE_VALUE && close) { close(networkHandle); networkHandle = INVALID_HANDLE_VALUE; }
        if (listener != INVALID_SOCKET) { closesocket(listener); listener = INVALID_SOCKET; }
        if (listener6 != INVALID_SOCKET) { closesocket(listener6); listener6 = INVALID_SOCKET; }
        if (socketThread.joinable()) socketThread.join(); if (flowThread.joinable()) flowThread.join(); if (networkThread.joinable()) networkThread.join(); if (acceptThread.joinable()) acceptThread.join(); if (acceptThread6.joinable()) acceptThread6.join();
        { std::lock_guard lock(workersMutex); for (SOCKET socket : clients) { shutdown(socket, SD_BOTH); closesocket(socket); } clients.clear(); }
        for (auto& worker : workers) if (worker.joinable()) worker.join(); workers.clear();
        WSACleanup(); if (module) FreeLibrary(module); module = nullptr;
    }
};

ProcessFilter::ProcessFilter(std::unique_ptr<Impl> implementation) : implementation_(std::move(implementation)) {}
ProcessFilter::~ProcessFilter() = default;

unsigned long long ProcessFilter::matchedFlows() const { return implementation_ ? implementation_->matched.load() : 0; }
unsigned long long ProcessFilter::redirectedPackets() const { return implementation_ ? implementation_->redirected.load() : 0; }
unsigned long long ProcessFilter::proxyReplies() const { return implementation_ ? implementation_->proxyReplies.load() : 0; }
unsigned long long ProcessFilter::acceptedConnections() const { return implementation_ ? implementation_->accepted.load() : 0; }
unsigned long long ProcessFilter::acceptAttempts() const { return implementation_ ? implementation_->acceptAttempts.load() : 0; }
unsigned long long ProcessFilter::natMisses() const { return implementation_ ? implementation_->natMisses.load() : 0; }
unsigned long long ProcessFilter::udpMatchedFlows() const { return implementation_ ? implementation_->udpMatched.load() : 0; }
unsigned long long ProcessFilter::udpSentPackets() const { return implementation_ ? implementation_->udpSent.load() : 0; }
unsigned long long ProcessFilter::udpReceivedPackets() const { return implementation_ ? implementation_->udpReceived.load() : 0; }
unsigned long long ProcessFilter::socketEvents() const { return implementation_ ? implementation_->socketEventCount.load() : 0; }
unsigned long long ProcessFilter::namedProcesses() const { return implementation_ ? implementation_->namedProcessCount.load() : 0; }
unsigned long long ProcessFilter::injectionFailures() const { return implementation_ ? implementation_->injectionFailureCount.load() : 0; }
unsigned long ProcessFilter::lastInjectionError() const { return implementation_ ? implementation_->injectionError.load() : 0; }

std::unique_ptr<ProcessFilter> ProcessFilter::start(std::vector<std::wstring> executableNames, TunnelClient& client, std::wstring& error) {
    auto implementation = std::make_unique<Impl>(); implementation->client = &client;
    for (auto& name : executableNames) { name = lower(std::filesystem::path(name).filename().wstring()); if (!name.empty()) implementation->names.push_back(std::move(name)); }
    if (implementation->names.empty()) { error = L"Добавьте хотя бы одно имя процесса, например Discord.exe"; return nullptr; }
    if (!implementation->initialize(error)) return nullptr;
    return std::unique_ptr<ProcessFilter>(new ProcessFilter(std::move(implementation)));
}
