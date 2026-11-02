#pragma once

#include "CodexUsageCore.h"
#include "CodexUsageConfig.h"

#include <optional>
#include <string>
#include <vector>

struct CodexAuthInfo {
    std::string accessToken;
    std::string accountId;
};

class CodexUsageFetcher {
public:
    UsageSnapshot Fetch(const CodexUsageConfig& config = {}) const;

private:
    std::optional<CodexAuthInfo> ReadAuthInfo(const CodexUsageConfig& config, std::wstring* errorMessage) const;
    std::optional<std::string> LoadFileUtf8(const std::wstring& path, std::wstring* errorMessage) const;
    std::vector<std::wstring> BuildAuthHeaders(const CodexAuthInfo& auth) const;
    std::optional<std::string> HttpGetUsageJson(const CodexAuthInfo& auth, const CodexUsageConfig& config, std::wstring* errorMessage) const;
    std::optional<std::string> HttpGetSubscriptionJson(const CodexAuthInfo& auth, const CodexUsageConfig& config, std::wstring* errorMessage) const;
    std::optional<std::string> HttpGetResetCreditsJson(const CodexAuthInfo& auth, const CodexUsageConfig& config, std::wstring* errorMessage) const;
};
