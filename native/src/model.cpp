#include "model.h"

#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
std::filesystem::path dataDirectory() {
    PWSTR raw = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) {
        result = std::filesystem::path(raw) / L"BigHeadVPNNative";
        CoTaskMemFree(raw);
    }
    return result;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string escape(const std::wstring& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : utf8(value)) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(hex[c >> 4]);
            result.push_back(hex[c & 15]);
        }
    }
    return result;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::wstring unescape(const std::string& value) {
    std::string bytes;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            int hi = hexValue(value[i + 1]), lo = hexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                bytes.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        bytes.push_back(value[i]);
    }
    return wide(bytes);
}

std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> result;
    size_t begin = 0;
    while (begin <= line.size()) {
        size_t end = line.find('|', begin);
        result.push_back(line.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

std::wstring decodeFragment(std::wstring text) {
    std::string bytes;
    std::string source = utf8(text);
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '%' && i + 2 < source.size()) {
            int hi = hexValue(source[i + 1]), lo = hexValue(source[i + 2]);
            if (hi >= 0 && lo >= 0) {
                bytes.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        bytes.push_back(source[i] == '+' ? ' ' : source[i]);
    }
    return wide(bytes);
}
}

std::wstring newId() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return std::to_wstring(GetTickCount64());
    wchar_t value[40]{};
    StringFromGUID2(id, value, static_cast<int>(std::size(value)));
    std::wstring result(value);
    if (result.size() > 2) result = result.substr(1, result.size() - 2);
    return result;
}

bool supportedProfile(const std::wstring& uri) {
    auto pos = uri.find(L"://");
    if (pos == std::wstring::npos) return false;
    std::wstring scheme = uri.substr(0, pos);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), towlower);
    return scheme == L"vless" || scheme == L"hysteria" ||
           scheme == L"hysteria2" || scheme == L"hy2";
}

std::wstring profileName(const std::wstring& uri) {
    auto hash = uri.find(L'#');
    if (hash != std::wstring::npos && hash + 1 < uri.size()) {
        auto decoded = decodeFragment(uri.substr(hash + 1));
        if (!decoded.empty()) return decoded;
    }
    auto scheme = uri.find(L"://");
    auto begin = scheme == std::wstring::npos ? 0 : scheme + 3;
    auto at = uri.find(L'@', begin);
    if (at != std::wstring::npos) begin = at + 1;
    auto end = uri.find_first_of(L":/?#", begin);
    auto host = uri.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
    return host.empty() ? L"Сервер" : host;
}

bool AppModel::load() {
    groups.clear(); profiles.clear(); selectedProfileId.clear(); filteredProcesses.clear();
    auto file = dataDirectory() / L"settings.dat";
    std::ifstream input(file, std::ios::binary);
    if (!input) return true;
    std::vector<unsigned char> encrypted((std::istreambuf_iterator<char>(input)), {});
    if (encrypted.empty()) return true;
    DATA_BLOB in{static_cast<DWORD>(encrypted.size()), encrypted.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    std::string content(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto f = fields(line);
        if (f.size() == 4 && f[0] == "G") groups.push_back({unescape(f[1]), unescape(f[2]), unescape(f[3])});
        else if (f.size() == 5 && f[0] == "P") profiles.push_back({unescape(f[1]), unescape(f[2]), unescape(f[3]), unescape(f[4])});
        else if (f.size() == 2 && f[0] == "S") selectedProfileId = unescape(f[1]);
        else if (f.size() == 2 && f[0] == "F") filteredProcesses.push_back(unescape(f[1]));
        else if (f.size() == 3 && f[0] == "L") {
            listenAddress = unescape(f[1]);
            unsigned value = 0;
            auto parsed = std::from_chars(f[2].data(), f[2].data() + f[2].size(), value);
            if (parsed.ec == std::errc{} && value > 0 && value <= 65535) listenPort = static_cast<unsigned short>(value);
        }
    }
    return true;
}

bool AppModel::save() const {
    std::ostringstream content;
    for (const auto& group : groups) content << "G|" << escape(group.id) << '|' << escape(group.name) << '|' << escape(group.url) << '\n';
    for (const auto& profile : profiles) content << "P|" << escape(profile.id) << '|' << escape(profile.groupId) << '|' << escape(profile.name) << '|' << escape(profile.uri) << '\n';
    content << "S|" << escape(selectedProfileId) << '\n';
    content << "L|" << escape(listenAddress) << '|' << listenPort << '\n';
    for (const auto& process : filteredProcesses) content << "F|" << escape(process) << '\n';
    auto plain = content.str();
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(plain.data())}, out{};
    if (!CryptProtectData(&in, L"Big Head VPN settings", nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    auto directory = dataDirectory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    auto temporary = directory / L"settings.tmp";
    auto target = directory / L"settings.dat";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(out.pbData), out.cbData);
    output.close();
    LocalFree(out.pbData);
    if (!output) return false;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::filesystem::remove(target, error);
        error.clear();
        std::filesystem::rename(temporary, target, error);
    }
    return !error;
}

void AppModel::replaceGroup(const SubscriptionGroup& group, std::vector<Profile> incoming) {
    deleteGroup(group.id);
    groups.push_back(group);
    for (auto& profile : incoming) {
        profile.groupId = group.id;
        profiles.push_back(std::move(profile));
    }
}

void AppModel::deleteGroup(const std::wstring& groupId) {
    groups.erase(std::remove_if(groups.begin(), groups.end(), [&](const auto& item) { return item.id == groupId; }), groups.end());
    profiles.erase(std::remove_if(profiles.begin(), profiles.end(), [&](const auto& item) { return item.groupId == groupId; }), profiles.end());
    if (std::none_of(profiles.begin(), profiles.end(), [&](const auto& item) { return item.id == selectedProfileId; })) selectedProfileId.clear();
}
