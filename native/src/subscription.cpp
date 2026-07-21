#include "subscription.h"

#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <memory>
#include <sstream>

namespace {
struct HandleCloser { void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); } };
using HttpHandle = std::unique_ptr<void, HandleCloser>;

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string trim(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) value.pop_back();
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
    return value.substr(begin);
}

std::wstring subscriptionTitle(const std::string& body) {
    static constexpr char prefix[] = "#profile-title:";
    std::istringstream lines(body);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.rfind(prefix, 0) == 0) return wide(trim(line.substr(sizeof(prefix) - 1)));
        if (!line.empty() && line[0] != '#') break;
    }
    return {};
}

bool looksLikeProfiles(const std::string& text) {
    return text.find("vless://") != std::string::npos || text.find("hysteria2://") != std::string::npos ||
           text.find("hysteria://") != std::string::npos || text.find("hy2://") != std::string::npos;
}

std::string decodeBase64(const std::string& body) {
    DWORD size = 0;
    if (!CryptStringToBinaryA(body.c_str(), static_cast<DWORD>(body.size()), CRYPT_STRING_BASE64_ANY,
        nullptr, &size, nullptr, nullptr)) return {};
    std::string decoded(size, '\0');
    if (!CryptStringToBinaryA(body.c_str(), static_cast<DWORD>(body.size()), CRYPT_STRING_BASE64_ANY,
        reinterpret_cast<BYTE*>(decoded.data()), &size, nullptr, nullptr)) return {};
    decoded.resize(size);
    return decoded;
}
}

std::vector<Profile> parseSubscriptionText(const std::string& body) {
    std::string text = body;
    if (!looksLikeProfiles(text)) {
        auto decoded = decodeBase64(trim(text));
        if (looksLikeProfiles(decoded)) text = std::move(decoded);
    }
    std::vector<Profile> result;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(line);
        auto uri = wide(line);
        if (!supportedProfile(uri)) continue;
        result.push_back({newId(), L"", profileName(uri), uri});
    }
    return result;
}

SubscriptionResult downloadSubscription(const std::wstring& url) {
    SubscriptionResult result;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[512]{}, path[4096]{};
    parts.lpszHostName = host; parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path; parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP)) {
        result.error = L"Нужна корректная ссылка HTTP или HTTPS";
        return result;
    }
    result.suggestedName.assign(host, parts.dwHostNameLength);
    HttpHandle session(WinHttpOpen(L"BigHeadVPNNative/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { result.error = L"Не удалось открыть WinHTTP"; return result; }
    WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 15000);
    HttpHandle connection(WinHttpConnect(session.get(), host, parts.nPort, 0));
    if (!connection) { result.error = L"Не удалось подключиться к серверу подписки"; return result; }
    std::wstring requestPath(path, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength && parts.lpszExtraInfo) requestPath.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (requestPath.empty()) requestPath = L"/";
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", requestPath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request || !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr)) {
        result.error = L"Сервер подписки не ответил";
        return result;
    }
    DWORD status = 0, statusSize = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        result.error = L"Сервер подписки вернул HTTP " + std::to_wstring(status);
        return result;
    }
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) { result.error = L"Ошибка чтения подписки"; return result; }
        if (!available) break;
        if (body.size() + available > 4 * 1024 * 1024) { result.error = L"Подписка слишком большая"; return result; }
        size_t offset = body.size(); body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), body.data() + offset, available, &read)) { result.error = L"Ошибка чтения подписки"; return result; }
        body.resize(offset + read);
    }
    result.profiles = parseSubscriptionText(body);
    auto title = subscriptionTitle(body);
    if (!title.empty()) result.suggestedName = std::move(title);
    if (result.profiles.empty()) result.error = L"В подписке нет поддерживаемых VLESS/Hysteria2-серверов";
    return result;
}
