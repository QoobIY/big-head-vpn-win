#pragma once

#include "schannel_tls.h"

#include <nghttp2/nghttp2.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class Http2Connection {
public:
    using DataHandler = std::function<void(const unsigned char*, size_t)>;

    Http2Connection() = default;
    ~Http2Connection();
    Http2Connection(const Http2Connection&) = delete;
    Http2Connection& operator=(const Http2Connection&) = delete;

    bool connect(const std::wstring& endpoint, unsigned short port, const std::string& authority,
        std::wstring& error);
    int32_t open(const std::string& path,
        const std::vector<std::pair<std::string, std::string>>& headers,
        std::vector<unsigned char> body, DataHandler handler, std::wstring& error);
    bool waitHeaders(int32_t streamId, unsigned& status, std::wstring& error, unsigned timeoutMs = 15000);
    bool waitClosed(int32_t streamId, std::wstring& error, unsigned timeoutMs = 15000);
    void close();

private:
    struct Stream;
    static nghttp2_ssize sendCallback(nghttp2_session*, const uint8_t*, size_t, int, void*);
    static nghttp2_ssize bodyCallback(nghttp2_session*, int32_t, uint8_t*, size_t, uint32_t*,
        nghttp2_data_source*, void*);
    static int headerCallback(nghttp2_session*, const nghttp2_frame*, const uint8_t*, size_t,
        const uint8_t*, size_t, uint8_t, void*);
    static int frameCallback(nghttp2_session*, const nghttp2_frame*, void*);
    static int dataCallback(nghttp2_session*, uint8_t, int32_t, const uint8_t*, size_t, void*);
    static int closeCallback(nghttp2_session*, int32_t, uint32_t, void*);
    std::shared_ptr<Stream> stream(int32_t id);
    void readLoop();
    void fail(std::wstring error);

    SchannelTls tls_;
    nghttp2_session* session_{};
    std::string authority_;
    std::mutex sessionMutex_;
    std::mutex streamsMutex_;
    std::unordered_map<int32_t, std::shared_ptr<Stream>> streams_;
    std::atomic_bool stopping_{};
    std::thread reader_;
    std::mutex errorMutex_;
    std::wstring connectionError_;
};
