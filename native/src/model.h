#pragma once

#include <string>
#include <vector>

struct Profile {
    std::wstring id;
    std::wstring groupId;
    std::wstring name;
    std::wstring uri;
};

struct SubscriptionGroup {
    std::wstring id;
    std::wstring name;
    std::wstring url;
};

enum class ProfileKind {
    Hysteria2,
    VlessXhttpTls,
    VlessGrpcTls,
    VlessVisionReality,
    VlessGrpcReality,
    Unsupported
};

struct AppModel {
    std::vector<SubscriptionGroup> groups;
    std::vector<Profile> profiles;
    std::wstring selectedProfileId;
    std::wstring listenAddress = L"127.0.0.1";
    unsigned short listenPort = 2080;
    std::vector<std::wstring> filteredProcesses;

    bool load();
    bool save() const;
    void replaceGroup(const SubscriptionGroup& group, std::vector<Profile> incoming);
    void deleteGroup(const std::wstring& groupId);
};

std::wstring newId();
std::wstring profileName(const std::wstring& uri);
bool supportedProfile(const std::wstring& uri);
ProfileKind profileKind(const std::wstring& uri);
std::wstring profileKindName(ProfileKind kind);
