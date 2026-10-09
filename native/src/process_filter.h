#pragma once

#include <memory>
#include <string>
#include <vector>

class TunnelClient;

class ProcessFilter {
public:
    static std::unique_ptr<ProcessFilter> start(
        std::vector<std::wstring> executableNames,
        TunnelClient& client,
        std::wstring& error);

    // Interrupt traffic without waiting for worker threads to finish.
    void requestStop();

    ~ProcessFilter();
    ProcessFilter(const ProcessFilter&) = delete;
    ProcessFilter& operator=(const ProcessFilter&) = delete;

    unsigned long long matchedFlows() const;
    unsigned long long redirectedPackets() const;
    unsigned long long proxyReplies() const;
    unsigned long long acceptedConnections() const;
    unsigned long long acceptAttempts() const;
    unsigned long long natMisses() const;
    unsigned long long udpMatchedFlows() const;
    unsigned long long udpSentPackets() const;
    unsigned long long udpReceivedPackets() const;
    unsigned long long tcpLateFlows() const;
    unsigned long long tcpLateMaxMs() const;
    unsigned long long udpLateFlows() const;
    unsigned long long udpLatePackets() const;
    unsigned long long udpLateMaxMs() const;
    unsigned long long udpResponseLastMs() const;
    unsigned long long udpResponseMaxMs() const;
    unsigned long long socketEvents() const;
    unsigned long long namedProcesses() const;
    unsigned long long injectionFailures() const;
    unsigned long lastInjectionError() const;

private:
    struct Impl;
    explicit ProcessFilter(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};
