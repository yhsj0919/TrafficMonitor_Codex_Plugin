#include "CodexUsageCore.h"
#include "CodexUsageVersion.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void AssertTrue(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void AssertEqual(int actual, int expected, const char* message) {
    if (actual != expected) {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

void AssertEqual(const std::wstring& actual, const std::wstring& expected, const char* message) {
    if (actual != expected) {
        std::wcerr << message << L": expected [" << expected << L"], got [" << actual << L"]\n";
        std::exit(1);
    }
}

void AssertEqual(long long actual, long long expected, const char* message) {
    if (actual != expected) {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

void AssertNotContains(const std::wstring& actual, wchar_t needle, const char* message) {
    if (actual.find(needle) != std::wstring::npos) {
        std::wcerr << message << L": unexpected character [" << needle << L"] in [" << actual << L"]\n";
        std::exit(1);
    }
}

void ParsesUsageWindows() {
    const std::string json = R"json({
        "email": "user@example.com",
        "plan_type": "pro",
        "rate_limit": {
            "primary_window": {
                "used_percent": 23,
                "limit_window_seconds": 18000,
                "reset_after_seconds": 3600,
                "reset_at": 1780980000
            },
            "secondary_window": {
                "used_percent": 61,
                "limit_window_seconds": 604800,
                "reset_after_seconds": 86400,
                "reset_at": 1781066400
            }
        }
    })json";

    std::wstring error;
    UsageSnapshot snapshot = ParseUsageJson(json, &error);

    AssertTrue(snapshot.success, "snapshot should parse");
    AssertEqual(snapshot.fiveHour.remainingPercent, 77, "five-hour remaining percent");
    AssertEqual(snapshot.weekly.remainingPercent, 39, "weekly remaining percent");
    AssertEqual(snapshot.email, L"user@example.com", "email");
    AssertEqual(snapshot.planType, L"pro", "plan type");
    AssertTrue(!snapshot.hasSubscriptionExpiry, "usage payload alone should not set subscription expiry");
}

void ParsesSubscriptionExpiry() {
    UsageSnapshot snapshot;
    snapshot.success = true;
    snapshot.planType = L"free";

    std::wstring error;
    AssertTrue(
        ApplySubscriptionJson(
            R"json({"plan_type":"plus","active_until":"2026-06-10T02:52:15Z","will_renew":true,"id":"sub_123"})json",
            &snapshot,
            &error),
        "subscription payload should parse");
    AssertTrue(error.empty(), "valid subscription payload should not set an error");
    AssertTrue(snapshot.hasSubscriptionExpiry, "subscription expiry should be available");
    AssertEqual(snapshot.activeUntilUnixSeconds, 1781059935LL, "active_until unix seconds");
    AssertTrue(snapshot.willRenew, "will_renew should be true");
    AssertEqual(snapshot.planType, L"plus", "subscription plan type overrides usage plan type");
    const std::wstring expiryText = FormatExpiryDateTime(1781059935LL);
    AssertEqual(static_cast<int>(expiryText.size()), 11, "expiry display should be MM-DD HH:MM");
    AssertTrue(expiryText[2] == L'-' && expiryText[5] == L' ' && expiryText[8] == L':',
               "expiry display should use MM-DD HH:MM layout");

    AssertTrue(ParseRfc3339ToUnixSeconds("2026-05-02T20:32:12+00:00").has_value(), "offset form should parse");
    AssertTrue(!ParseRfc3339ToUnixSeconds("not-a-date").has_value(), "invalid date should fail");
    AssertTrue(
        !ApplySubscriptionJson(R"json({"plan_type":"plus","will_renew":false})json", &snapshot, &error),
        "missing active_until should fail");
}

void ParsesResetCredits() {
    UsageSnapshot snapshot;
    snapshot.success = true;

    std::wstring error;
    AssertTrue(
        ApplyResetCreditsJson(
            R"json({
                "available_count": 2,
                "credits": [
                    {
                        "status": "redeemed",
                        "expires_at": "2099-01-01T00:00:00Z"
                    },
                    {
                        "status": "available",
                        "expires_at": "2099-07-27T07:50:13Z"
                    },
                    {
                        "status": "available",
                        "expires_at": "2099-07-18T00:33:49.661454Z"
                    }
                ]
            })json",
            &snapshot,
            &error),
        "reset credits payload should parse");
    AssertTrue(error.empty(), "valid reset credits payload should not set an error");
    AssertTrue(snapshot.hasResetCredits, "reset credits should be available");
    AssertEqual(snapshot.resetCreditAvailableCount, 2, "available reset credit count");
    AssertTrue(snapshot.hasNearestResetCreditExpiry, "nearest reset credit expiry should be set");
    AssertEqual(snapshot.resetCreditExpiresUnixSeconds.size() == 2 ? 1 : 0, 1, "two future available credits");
    AssertTrue(
        snapshot.resetCreditExpiresUnixSeconds[0] < snapshot.resetCreditExpiresUnixSeconds[1],
        "reset credit expiries should be sorted ascending");
    AssertEqual(
        snapshot.nearestResetCreditExpiresUnixSeconds,
        snapshot.resetCreditExpiresUnixSeconds.front(),
        "nearest expiry should be the earliest credit");
    AssertEqual(
        FormatResetCreditDisplay(snapshot),
        FormatExpiryDateTime(snapshot.nearestResetCreditExpiresUnixSeconds),
        "display should show only nearest reset credit expiry");

    UsageSnapshot emptySnapshot;
    emptySnapshot.success = true;
    AssertTrue(ApplyResetCreditsJson(R"json({"available_count":0,"credits":[]})json", &emptySnapshot, &error),
               "empty reset credits payload should parse");
    AssertEqual(FormatResetCreditDisplay(emptySnapshot), L"--", "no credits should display placeholder");
}

void ClassifiesPrimaryWindowWithoutSecondaryWindow() {
    const std::string json = R"json({
        "rate_limit": {
            "primary_window": {
                "used_percent": 4,
                "limit_window_seconds": 604800,
                "reset_after_seconds": 580302,
                "reset_at": 1780980000
            }
        }
    })json";

    std::wstring error;
    UsageSnapshot snapshot = ParseUsageJson(json, &error);

    AssertTrue(snapshot.success, "snapshot should parse without secondary window");
    AssertTrue(error.empty(), "missing secondary window should not set an error");
    AssertEqual(snapshot.fiveHour.remainingPercent, -1, "missing five-hour window should be unavailable");
    AssertEqual(snapshot.weekly.remainingPercent, 96, "primary weekly remaining percent");
    AssertEqual(snapshot.weekly.resetAtUnixSeconds, 1780980000LL, "primary weekly reset_at");
    const std::wstring tooltip = BuildUsageTooltip(snapshot);
    AssertTrue(tooltip.find(L"Codex: 5\u5c0f\u65f6\u5269\u4f59 --") != std::wstring::npos,
               "tooltip should mark missing five-hour window unavailable");
    AssertTrue(tooltip.find(L"Codex: \u672c\u5468\u5269\u4f59 96% (") != std::wstring::npos,
               "tooltip should include weekly remaining");
    AssertTrue(tooltip.find(L"\u91cd\u7f6e)") != std::wstring::npos,
               "tooltip should show absolute weekly reset time");
    AssertTrue(tooltip.find(L"\u540e\u91cd\u7f6e") == std::wstring::npos,
               "tooltip should not use relative reset countdown");
}

void FormatsDisplayText() {
    AssertEqual(FormatRemainingPercent(77), L"77%", "remaining percent text");
    AssertEqual(FormatRemainingPercent(-1), L"--", "negative percent text");
    AssertEqual(FormatRemainingPercent(101), L"--", "out-of-range percent text");
}

void BuildsMultiLineSuccessTooltip() {
    UsageSnapshot snapshot;
    snapshot.success = true;
    snapshot.planType = L"pro";
    snapshot.fiveHour.remainingPercent = 77;
    snapshot.fiveHour.resetAtUnixSeconds = 1780980000LL;
    snapshot.weekly.remainingPercent = 39;
    snapshot.weekly.resetAtUnixSeconds = 1781066400LL;
    snapshot.hasSubscriptionExpiry = true;
    snapshot.activeUntilUnixSeconds = 1781059935LL;
    snapshot.willRenew = true;
    snapshot.hasResetCredits = true;
    snapshot.resetCreditAvailableCount = 2;
    snapshot.hasNearestResetCreditExpiry = true;
    snapshot.nearestResetCreditExpiresUnixSeconds = 1784339629LL;
    snapshot.resetCreditExpiresUnixSeconds = {1784339629LL, 1785142213LL};

    const std::wstring tooltip = BuildUsageTooltip(snapshot);
    AssertTrue(tooltip.find(L"Codex: 5\u5c0f\u65f6\u5269\u4f59 77% (") != std::wstring::npos,
               "tooltip should include five-hour remaining");
    AssertTrue(tooltip.find(L"Codex: \u672c\u5468\u5269\u4f59 39% (") != std::wstring::npos,
               "tooltip should include weekly remaining");
    AssertTrue(tooltip.find(L"\u540e\u91cd\u7f6e") == std::wstring::npos,
               "tooltip should not use relative reset countdown");
    AssertTrue(tooltip.find(FormatExpiryDateTime(1780980000LL) + L"\u91cd\u7f6e") != std::wstring::npos,
               "tooltip should include absolute five-hour reset time");
    AssertTrue(tooltip.find(FormatExpiryDateTime(1781066400LL) + L"\u91cd\u7f6e") != std::wstring::npos,
               "tooltip should include absolute weekly reset time");
    AssertTrue(tooltip.find(L"\u8ba1\u5212: pro") != std::wstring::npos, "tooltip should include plan type");
    AssertTrue(tooltip.find(L"\u8ba2\u9605\u5230\u671f: ") != std::wstring::npos, "tooltip should include subscription expiry");
    AssertTrue(tooltip.find(L"(\u81ea\u52a8\u7eed\u8d39)") != std::wstring::npos, "tooltip should include renew status");
    AssertTrue(tooltip.find(L"\u91cd\u7f6e\u5361: 2\u5f20") != std::wstring::npos, "tooltip should include reset credit count");
}

void HasGeneratedVersion() {
    AssertTrue(std::wstring(CODEX_USAGE_VERSION_WIDE).size() > 0, "generated version should not be empty");
}

}  // namespace

void RunCodexConfigTests();

int main() {
    RunCodexConfigTests();
    ParsesUsageWindows();
    ParsesSubscriptionExpiry();
    ParsesResetCredits();
    ClassifiesPrimaryWindowWithoutSecondaryWindow();
    FormatsDisplayText();
    BuildsMultiLineSuccessTooltip();
    HasGeneratedVersion();
    return 0;
}
