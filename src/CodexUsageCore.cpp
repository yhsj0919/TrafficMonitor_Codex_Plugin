#include "CodexUsageCore.h"

#include "JsonLite.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <ctime>

namespace {

constexpr int kFiveHourWindowSeconds = 5 * 60 * 60;
constexpr int kWeeklyWindowSeconds = 7 * 24 * 60 * 60;

bool ParseIntFragment(std::string_view text, int* value) {
    if (value == nullptr || text.empty() || text.size() > 9) {
        return false;
    }
    int parsed = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        parsed = parsed * 10 + (ch - '0');
    }
    *value = parsed;
    return true;
}

std::wstring FormatLocalDateTime(long long unixSeconds) {
    if (unixSeconds <= 0) {
        return L"--";
    }

    const time_t raw = static_cast<time_t>(unixSeconds);
    tm localTime{};
    if (localtime_s(&localTime, &raw) != 0) {
        return L"--";
    }

    wchar_t buffer[32]{};
    if (wcsftime(buffer, 32, L"%m-%d %H:%M", &localTime) == 0) {
        return L"--";
    }
    return buffer;
}

bool ExtractWindow(const jsonlite::Value* windowNode, UsageWindow* output) {
    if (windowNode == nullptr || output == nullptr) {
        return false;
    }

    const jsonlite::Value* usedPercent = windowNode->Find("used_percent");
    const jsonlite::Value* limitWindowSeconds = windowNode->Find("limit_window_seconds");
    const jsonlite::Value* resetAfterSeconds = windowNode->Find("reset_after_seconds");
    const jsonlite::Value* resetAt = windowNode->Find("reset_at");
    if (usedPercent == nullptr || limitWindowSeconds == nullptr || resetAfterSeconds == nullptr || resetAt == nullptr) {
        return false;
    }

    auto used = usedPercent->AsInt();
    auto limit = limitWindowSeconds->AsInt();
    auto resetAfter = resetAfterSeconds->AsInt();
    auto resetAtValue = resetAt->AsNumber();
    if (!used.has_value() || !limit.has_value() || !resetAfter.has_value() || !resetAtValue.has_value()) {
        return false;
    }

    output->usedPercent = std::clamp(*used, 0, 100);
    output->remainingPercent = 100 - output->usedPercent;
    output->windowSeconds = std::max(*limit, 0);
    output->resetAfterSeconds = std::max(*resetAfter, 0);
    output->resetAtUnixSeconds = static_cast<long long>(*resetAtValue);
    return true;
}

void AssignWindowByDuration(const UsageWindow& window, UsageSnapshot* snapshot) {
    if (snapshot == nullptr) {
        return;
    }

    if (window.windowSeconds == kFiveHourWindowSeconds) {
        snapshot->fiveHour = window;
    } else if (window.windowSeconds == kWeeklyWindowSeconds) {
        snapshot->weekly = window;
    }
}

}  // namespace

std::wstring Utf8ToWide(const std::string& input) {
    if (input.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }

    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), size);
    return output;
}

UsageSnapshot ParseUsageJson(const std::string& jsonText, std::wstring* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    UsageSnapshot snapshot;
    jsonlite::Parser parser(jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"usage JSON parse failed: " + Utf8ToWide(parser.Error());
        }
        return snapshot;
    }

    const jsonlite::Value* email = root->Find("email");
    const jsonlite::Value* planType = root->Find("plan_type");
    const jsonlite::Value* rateLimit = root->Find("rate_limit");
    const jsonlite::Value* primaryWindow = rateLimit != nullptr ? rateLimit->Find("primary_window") : nullptr;
    const jsonlite::Value* secondaryWindow = rateLimit != nullptr ? rateLimit->Find("secondary_window") : nullptr;
    UsageWindow primaryUsageWindow;
    if (!ExtractWindow(primaryWindow, &primaryUsageWindow)) {
        if (errorMessage != nullptr) {
            *errorMessage = L"usage payload missing primary rate_limit window";
        }
        return snapshot;
    }
    AssignWindowByDuration(primaryUsageWindow, &snapshot);

    if (secondaryWindow != nullptr) {
        UsageWindow secondaryUsageWindow;
        if (ExtractWindow(secondaryWindow, &secondaryUsageWindow)) {
            AssignWindowByDuration(secondaryUsageWindow, &snapshot);
        }
    }

    if (auto emailString = email != nullptr ? email->AsString() : std::nullopt; emailString.has_value()) {
        snapshot.email = Utf8ToWide(std::string(*emailString));
    }
    if (auto planTypeString = planType != nullptr ? planType->AsString() : std::nullopt; planTypeString.has_value()) {
        snapshot.planType = Utf8ToWide(std::string(*planTypeString));
    }

    snapshot.success = true;
    return snapshot;
}

std::optional<long long> ParseRfc3339ToUnixSeconds(std::string_view text) {
    // Accepts YYYY-MM-DDTHH:MM:SS[.fff]Z or YYYY-MM-DDTHH:MM:SS[.fff]±HH:MM
    if (text.size() < 20) {
        return std::nullopt;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!ParseIntFragment(text.substr(0, 4), &year) || text[4] != '-' ||
        !ParseIntFragment(text.substr(5, 2), &month) || text[7] != '-' ||
        !ParseIntFragment(text.substr(8, 2), &day) || (text[10] != 'T' && text[10] != 't') ||
        !ParseIntFragment(text.substr(11, 2), &hour) || text[13] != ':' ||
        !ParseIntFragment(text.substr(14, 2), &minute) || text[16] != ':' ||
        !ParseIntFragment(text.substr(17, 2), &second)) {
        return std::nullopt;
    }

    size_t index = 19;
    if (index < text.size() && text[index] == '.') {
        ++index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            ++index;
        }
    }
    if (index >= text.size()) {
        return std::nullopt;
    }

    int offsetMinutes = 0;
    if (text[index] == 'Z' || text[index] == 'z') {
        if (index + 1 != text.size()) {
            return std::nullopt;
        }
    } else if (text[index] == '+' || text[index] == '-') {
        const bool negative = text[index] == '-';
        if (index + 6 != text.size() || text[index + 3] != ':') {
            return std::nullopt;
        }
        int offsetHour = 0;
        int offsetMinute = 0;
        if (!ParseIntFragment(text.substr(index + 1, 2), &offsetHour) ||
            !ParseIntFragment(text.substr(index + 4, 2), &offsetMinute)) {
            return std::nullopt;
        }
        offsetMinutes = offsetHour * 60 + offsetMinute;
        if (negative) {
            offsetMinutes = -offsetMinutes;
        }
    } else {
        return std::nullopt;
    }

    tm utcTime{};
    utcTime.tm_year = year - 1900;
    utcTime.tm_mon = month - 1;
    utcTime.tm_mday = day;
    utcTime.tm_hour = hour;
    utcTime.tm_min = minute;
    utcTime.tm_sec = second;
    const time_t utcSeconds = _mkgmtime(&utcTime);
    if (utcSeconds < 0) {
        return std::nullopt;
    }
    return static_cast<long long>(utcSeconds) - static_cast<long long>(offsetMinutes) * 60;
}

bool ApplySubscriptionJson(const std::string& jsonText, UsageSnapshot* snapshot, std::wstring* errorMessage) {
    if (snapshot == nullptr) {
        return false;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    jsonlite::Parser parser(jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"subscription JSON parse failed: " + Utf8ToWide(parser.Error());
        }
        return false;
    }

    const jsonlite::Value* activeUntil = root->Find("active_until");
    auto activeUntilString = activeUntil != nullptr ? activeUntil->AsString() : std::nullopt;
    if (!activeUntilString.has_value() || activeUntilString->empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"subscription payload missing active_until";
        }
        return false;
    }

    auto unixSeconds = ParseRfc3339ToUnixSeconds(*activeUntilString);
    if (!unixSeconds.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"subscription active_until is invalid";
        }
        return false;
    }

    snapshot->activeUntilUnixSeconds = *unixSeconds;
    snapshot->hasSubscriptionExpiry = true;

    if (const jsonlite::Value* willRenew = root->Find("will_renew"); willRenew != nullptr) {
        if (auto renew = willRenew->AsBool(); renew.has_value()) {
            snapshot->willRenew = *renew;
        }
    }

    if (const jsonlite::Value* planType = root->Find("plan_type"); planType != nullptr) {
        if (auto planTypeString = planType->AsString(); planTypeString.has_value() && !planTypeString->empty()) {
            snapshot->planType = Utf8ToWide(std::string(*planTypeString));
        }
    }

    return true;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

std::optional<long long> ParseCreditTimestamp(const jsonlite::Value* node) {
    if (node == nullptr) {
        return std::nullopt;
    }
    if (auto asString = node->AsString(); asString.has_value() && !asString->empty()) {
        return ParseRfc3339ToUnixSeconds(*asString);
    }
    if (auto asNumber = node->AsNumber(); asNumber.has_value()) {
        const double value = *asNumber;
        if (value <= 0) {
            return std::nullopt;
        }
        // Some clients emit milliseconds; normalize large values to seconds.
        if (value > 100000000000.0) {
            return static_cast<long long>(value / 1000.0);
        }
        return static_cast<long long>(value);
    }
    return std::nullopt;
}

bool ApplyResetCreditsJson(const std::string& jsonText, UsageSnapshot* snapshot, std::wstring* errorMessage) {
    if (snapshot == nullptr) {
        return false;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    jsonlite::Parser parser(jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"reset credits JSON parse failed: " + Utf8ToWide(parser.Error());
        }
        return false;
    }

    const long long nowUnixSeconds = static_cast<long long>(std::time(nullptr));
    std::vector<long long> expiresAtValues;
    int availableWithoutExpiry = 0;

    if (const jsonlite::Value* credits = root->Find("credits"); credits != nullptr) {
        const auto* creditsArray = credits->AsArray();
        if (creditsArray == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = L"reset credits payload credits is not an array";
            }
            return false;
        }

        for (const jsonlite::Value& credit : *creditsArray) {
            const jsonlite::Value* status = credit.Find("status");
            auto statusString = status != nullptr ? status->AsString() : std::nullopt;
            if (!statusString.has_value() || !EqualsIgnoreCase(*statusString, "available")) {
                continue;
            }

            auto expiresAt = ParseCreditTimestamp(credit.Find("expires_at"));
            if (!expiresAt.has_value()) {
                ++availableWithoutExpiry;
                continue;
            }
            if (*expiresAt <= nowUnixSeconds) {
                continue;
            }
            expiresAtValues.push_back(*expiresAt);
        }
    }

    std::sort(expiresAtValues.begin(), expiresAtValues.end());

    int availableCount = static_cast<int>(expiresAtValues.size()) + availableWithoutExpiry;
    if (const jsonlite::Value* availableCountNode = root->Find("available_count"); availableCountNode != nullptr) {
        if (auto reported = availableCountNode->AsInt(); reported.has_value() && *reported >= 0) {
            availableCount = std::max(availableCount, *reported);
        }
    }

    snapshot->hasResetCredits = true;
    snapshot->resetCreditAvailableCount = availableCount;
    snapshot->resetCreditExpiresUnixSeconds = std::move(expiresAtValues);
    snapshot->hasNearestResetCreditExpiry = !snapshot->resetCreditExpiresUnixSeconds.empty();
    snapshot->nearestResetCreditExpiresUnixSeconds =
        snapshot->hasNearestResetCreditExpiry ? snapshot->resetCreditExpiresUnixSeconds.front() : 0;
    return true;
}

std::wstring FormatRemainingPercent(int remainingPercent) {
    if (remainingPercent < 0 || remainingPercent > 100) {
        return L"--";
    }
    return std::to_wstring(remainingPercent) + L"%";
}

std::wstring FormatExpiryDateTime(long long unixSeconds) {
    return FormatLocalDateTime(unixSeconds);
}

std::wstring FormatResetCreditDisplay(const UsageSnapshot& snapshot) {
    if (!snapshot.hasResetCredits || !snapshot.hasNearestResetCreditExpiry) {
        return L"--";
    }
    return FormatExpiryDateTime(snapshot.nearestResetCreditExpiresUnixSeconds);
}

std::wstring BuildUsageTooltip(const UsageSnapshot& snapshot) {
    if (!snapshot.success) {
        if (snapshot.errorMessage.empty()) {
            return L"Codex Usage: no data";
        }
        return L"Codex Usage error: " + snapshot.errorMessage;
    }

    const auto formatWindow = [](const UsageWindow& window) {
        const std::wstring remaining = FormatRemainingPercent(window.remainingPercent);
        if (window.remainingPercent < 0) {
            return remaining;
        }
        if (window.resetAtUnixSeconds <= 0) {
            return remaining;
        }
        return remaining + L" (" + FormatExpiryDateTime(window.resetAtUnixSeconds) + L"\u91cd\u7f6e)";
    };

    std::wstring text = L"Codex: 5\u5c0f\u65f6\u5269\u4f59 " + formatWindow(snapshot.fiveHour);
    text += L"\nCodex: \u672c\u5468\u5269\u4f59 " + formatWindow(snapshot.weekly);

    if (!snapshot.planType.empty() || snapshot.hasSubscriptionExpiry) {
        text += L"\n";
        if (!snapshot.planType.empty()) {
            text += L"\u8ba1\u5212: " + snapshot.planType;
        }
        if (snapshot.hasSubscriptionExpiry) {
            if (!snapshot.planType.empty()) {
                text += L"\n";
            }
            text += L"\u8ba2\u9605\u5230\u671f: " + FormatExpiryDateTime(snapshot.activeUntilUnixSeconds);
            text += snapshot.willRenew ? L" (\u81ea\u52a8\u7eed\u8d39)" : L" (\u4e0d\u7eed\u8d39)";
        }
    }

    if (snapshot.hasResetCredits) {
        text += L"\n\u91cd\u7f6e\u5361: " + std::to_wstring(snapshot.resetCreditAvailableCount) + L"\u5f20";
        for (long long expiresAt : snapshot.resetCreditExpiresUnixSeconds) {
            text += L"\n  " + FormatExpiryDateTime(expiresAt);
        }
    }
    return text;
}
