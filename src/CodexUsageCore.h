#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct UsageWindow {
    int usedPercent = 0;
    int remainingPercent = -1;
    int windowSeconds = 0;
    int resetAfterSeconds = 0;
    long long resetAtUnixSeconds = 0;
};

struct UsageSnapshot {
    bool success = false;
    std::wstring email;
    std::wstring planType;
    std::wstring errorMessage;
    UsageWindow fiveHour;
    UsageWindow weekly;
    bool hasSubscriptionExpiry = false;
    long long activeUntilUnixSeconds = 0;
    bool willRenew = false;
    bool hasResetCredits = false;
    int resetCreditAvailableCount = 0;
    bool hasNearestResetCreditExpiry = false;
    long long nearestResetCreditExpiresUnixSeconds = 0;
    std::vector<long long> resetCreditExpiresUnixSeconds;
};

std::wstring Utf8ToWide(const std::string& input);
UsageSnapshot ParseUsageJson(const std::string& jsonText, std::wstring* errorMessage);
bool ApplySubscriptionJson(const std::string& jsonText, UsageSnapshot* snapshot, std::wstring* errorMessage);
bool ApplyResetCreditsJson(const std::string& jsonText, UsageSnapshot* snapshot, std::wstring* errorMessage);
std::optional<long long> ParseRfc3339ToUnixSeconds(std::string_view text);
std::wstring FormatRemainingPercent(int remainingPercent);
std::wstring FormatExpiryDateTime(long long unixSeconds);
std::wstring FormatResetCreditDisplay(const UsageSnapshot& snapshot);
std::wstring BuildUsageTooltip(const UsageSnapshot& snapshot);
