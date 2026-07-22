#include "http2_connection.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>

struct Http2Connection::Stream {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<unsigned char> body;
    size_t bodyOffset{};
    DataHandler handler;
    unsigned status{};
    bool headersDone{};
    bool closed{};
    uint32_t closeCode{};
};

namespace {
nghttp2_nv header(std::string& name, std::string& value) {
    return {reinterpret_cast<uint8_t*>(name.data()), reinterpret_cast<uint8_t*>(value.data()),
        name.size(), value.size(), NGHTTP2_NV_FLAG_NONE};
}

std::wstring nghttpError(const wchar_t* action, int code) {
    const char* detail = nghttp2_strerror(code);
    int length = detail ? MultiByteToWideChar(CP_UTF8, 0, detail, -1, nullptr, 0) : 0;
    std::wstring converted(length > 0 ? static_cast<size_t>(length) : 0, L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, detail, -1, converted.data(), length);
        if (!converted.empty() && converted.back() == L'\0') converted.pop_back();
    }
    return std::wstring(action) + L": " + converted;
}
}

Http2Connection::~Http2Connection() { close(); }

bool Http2Connection::connect(const std::wstring& endpoint, unsigned short port,
    const std::string& authority, std::wstring& error) {
    close();
    stopping_ = false;
    authority_ = authority;
    if (!tls_.connect(endpoint, port, error)) return false;
    nghttp2_session_callbacks* callbacks{};
    if (nghttp2_session_callbacks_new(&callbacks) != 0) { error = L"HTTP/2: не удалось создать callbacks"; return false; }
    nghttp2_session_callbacks_set_send_callback2(callbacks, sendCallback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, headerCallback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, frameCallback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, dataCallback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, closeCallback);
    int result = nghttp2_session_client_new(&session_, callbacks, this);
    nghttp2_session_callbacks_del(callbacks);
    if (result != 0) { error = nghttpError(L"HTTP/2: создание сессии", result); tls_.close(); return false; }
    nghttp2_settings_entry settings[]{
        {NGHTTP2_SETTINGS_ENABLE_PUSH, 0},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 16U * 1024U * 1024U}
    };
    result = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings, std::size(settings));
    if (result == 0) result = nghttp2_session_send(session_);
    if (result != 0) { error = nghttpError(L"HTTP/2: отправка SETTINGS", result); close(); return false; }
    reader_ = std::thread([this] { readLoop(); });
    return true;
}

int32_t Http2Connection::open(const std::string& path,
    const std::vector<std::pair<std::string, std::string>>& extraHeaders,
    std::vector<unsigned char> body, DataHandler handler, std::wstring& error) {
    if (!session_ || stopping_) { error = L"HTTP/2-соединение уже закрыто"; return -1; }
    auto request = std::make_shared<Stream>();
    request->body = std::move(body);
    request->handler = std::move(handler);
    std::vector<std::pair<std::string, std::string>> values{{":method", "GET"}, {":scheme", "https"},
        {":authority", authority_}, {":path", path}};
    values.insert(values.end(), extraHeaders.begin(), extraHeaders.end());
    std::vector<nghttp2_nv> fields;
    fields.reserve(values.size());
    for (auto& [name, value] : values) fields.push_back(header(name, value));
    nghttp2_data_provider provider{};
    nghttp2_data_provider* providerPointer = nullptr;
    if (!request->body.empty()) {
        provider.source.ptr = request.get();
        provider.read_callback = bodyCallback;
        providerPointer = &provider;
    }
    std::lock_guard sessionLock(sessionMutex_);
    int32_t id = nghttp2_submit_request(session_, nullptr, fields.data(), fields.size(),
        providerPointer, request.get());
    if (id < 0) { error = nghttpError(L"HTTP/2: создание запроса", id); return -1; }
    {
        std::lock_guard streamsLock(streamsMutex_);
        streams_[id] = request;
    }
    int result = nghttp2_session_send(session_);
    if (result != 0) { error = nghttpError(L"HTTP/2: отправка запроса", result); return -1; }
    return id;
}

std::shared_ptr<Http2Connection::Stream> Http2Connection::stream(int32_t id) {
    std::lock_guard lock(streamsMutex_);
    auto found = streams_.find(id);
    return found == streams_.end() ? nullptr : found->second;
}

bool Http2Connection::waitHeaders(int32_t id, unsigned& status, std::wstring& error, unsigned timeoutMs) {
    auto request = stream(id);
    if (!request) { error = L"HTTP/2: поток не найден"; return false; }
    std::unique_lock lock(request->mutex);
    if (!request->changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            return request->headersDone || request->closed || stopping_.load(); })) {
        error = L"HTTP/2: сервер не прислал заголовки вовремя";
        return false;
    }
    status = request->status;
    if (!request->headersDone) {
        std::lock_guard errorLock(errorMutex_);
        error = connectionError_.empty() ? L"HTTP/2: поток закрыт до ответа" : connectionError_;
        return false;
    }
    return true;
}

bool Http2Connection::waitClosed(int32_t id, std::wstring& error, unsigned timeoutMs) {
    auto request = stream(id);
    if (!request) { error = L"HTTP/2: поток не найден"; return false; }
    std::unique_lock lock(request->mutex);
    if (!request->changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            return request->closed || stopping_.load(); })) {
        error = L"HTTP/2: запрос не завершился вовремя";
        return false;
    }
    if (request->closeCode != NGHTTP2_NO_ERROR) {
        error = L"HTTP/2: сервер сбросил поток, код " + std::to_wstring(request->closeCode);
        return false;
    }
    return request->closed;
}

nghttp2_ssize Http2Connection::sendCallback(nghttp2_session*, const uint8_t* data, size_t length,
    int, void* userData) {
    auto* self = static_cast<Http2Connection*>(userData);
    std::wstring error;
    if (!self->tls_.write({data, length}, error)) { self->fail(std::move(error)); return NGHTTP2_ERR_CALLBACK_FAILURE; }
    return static_cast<nghttp2_ssize>(length);
}

nghttp2_ssize Http2Connection::bodyCallback(nghttp2_session*, int32_t, uint8_t* buffer, size_t length,
    uint32_t* flags, nghttp2_data_source* source, void*) {
    auto* request = static_cast<Stream*>(source->ptr);
    size_t available = request->body.size() - request->bodyOffset;
    size_t copied = std::min(length, available);
    if (copied) std::memcpy(buffer, request->body.data() + request->bodyOffset, copied);
    request->bodyOffset += copied;
    if (request->bodyOffset == request->body.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
    return static_cast<nghttp2_ssize>(copied);
}

int Http2Connection::headerCallback(nghttp2_session*, const nghttp2_frame* frame,
    const uint8_t* name, size_t nameLength, const uint8_t* value, size_t valueLength, uint8_t, void* userData) {
    if (frame->hd.type != NGHTTP2_HEADERS || nameLength != 7 || std::memcmp(name, ":status", 7) != 0) return 0;
    auto request = static_cast<Http2Connection*>(userData)->stream(frame->hd.stream_id);
    if (!request) return 0;
    unsigned status{};
    for (size_t i = 0; i < valueLength && value[i] >= '0' && value[i] <= '9'; ++i) status = status * 10 + value[i] - '0';
    std::lock_guard lock(request->mutex);
    request->status = status;
    return 0;
}

int Http2Connection::frameCallback(nghttp2_session*, const nghttp2_frame* frame, void* userData) {
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_RESPONSE) return 0;
    auto request = static_cast<Http2Connection*>(userData)->stream(frame->hd.stream_id);
    if (!request) return 0;
    { std::lock_guard lock(request->mutex); request->headersDone = true; }
    request->changed.notify_all();
    return 0;
}

int Http2Connection::dataCallback(nghttp2_session*, uint8_t, int32_t id,
    const uint8_t* data, size_t length, void* userData) {
    auto request = static_cast<Http2Connection*>(userData)->stream(id);
    if (request && request->handler) request->handler(data, length);
    return 0;
}

int Http2Connection::closeCallback(nghttp2_session*, int32_t id, uint32_t code, void* userData) {
    auto request = static_cast<Http2Connection*>(userData)->stream(id);
    if (!request) return 0;
    { std::lock_guard lock(request->mutex); request->closed = true; request->closeCode = code; }
    request->changed.notify_all();
    return 0;
}

void Http2Connection::readLoop() {
    while (!stopping_) {
        std::vector<unsigned char> data;
        std::wstring error;
        if (!tls_.read(data, error)) { if (!stopping_) fail(std::move(error)); break; }
        std::lock_guard lock(sessionMutex_);
        nghttp2_ssize consumed = nghttp2_session_mem_recv(session_, data.data(), data.size());
        if (consumed < 0) { fail(nghttpError(L"HTTP/2: ошибка входящего кадра", static_cast<int>(consumed))); break; }
        int sent = nghttp2_session_send(session_);
        if (sent != 0) { fail(nghttpError(L"HTTP/2: ошибка ответа", sent)); break; }
    }
}

void Http2Connection::fail(std::wstring error) {
    { std::lock_guard lock(errorMutex_); if (connectionError_.empty()) connectionError_ = std::move(error); }
    stopping_ = true;
    std::lock_guard lock(streamsMutex_);
    for (auto& [id, request] : streams_) { (void)id; request->changed.notify_all(); }
}

void Http2Connection::close() {
    stopping_ = true;
    tls_.close();
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) reader_.join();
    if (session_) { nghttp2_session_del(session_); session_ = nullptr; }
    std::lock_guard lock(streamsMutex_);
    streams_.clear();
}
