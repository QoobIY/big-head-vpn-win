#pragma once

#include "model.h"

#include <string>
#include <vector>

struct SubscriptionResult {
    std::wstring error;
    std::wstring suggestedName;
    std::vector<Profile> profiles;
};

SubscriptionResult downloadSubscription(const std::wstring& url);
std::vector<Profile> parseSubscriptionText(const std::string& body);
std::wstring normalizeProfileUri(std::wstring uri);
