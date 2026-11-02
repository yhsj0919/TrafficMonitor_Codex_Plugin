#include "CodexUsageFetcher.h"
#include "CodexUsageOptions.h"
#include "CodexUsageVersion.h"

#include "PluginInterface.h"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace {

class CodexUsageItem final : public IPluginItem {
public:
    enum class Kind {
        FiveHour,
        Weekly,
        SubscriptionExpiry,
        ResetCreditExpiry,
    };

    CodexUsageItem(Kind kind, const wchar_t* name, const wchar_t* id, const wchar_t* label, const wchar_t* sample)
        : kind_(kind), name_(name), id_(id), label_(label), sample_(sample) {}

    const wchar_t* GetItemName() const override { return name_; }
    const wchar_t* GetItemId() const override { return id_; }
    const wchar_t* GetItemLableText() const override { return label_; }
    const wchar_t* GetItemValueText() const override { return valueText_.c_str(); }
    const wchar_t* GetItemValueSampleText() const override { return sample_; }
    int IsDrawResourceUsageGraph() const override {
        return kind_ == Kind::FiveHour || kind_ == Kind::Weekly ? 1 : 0;
    }
    float GetResourceUsageGraphValue() const override {
        if (remainingPercent_ < 0) {
            return 0.0f;
        }
        return static_cast<float>(remainingPercent_) / 100.0f;
    }

    void SetSnapshot(const UsageSnapshot& snapshot) {
        if (!snapshot.success) {
            remainingPercent_ = -1;
            valueText_ = L"--";
            return;
        }

        if (kind_ == Kind::SubscriptionExpiry) {
            remainingPercent_ = -1;
            valueText_ = snapshot.hasSubscriptionExpiry
                ? FormatExpiryDateTime(snapshot.activeUntilUnixSeconds)
                : L"--";
            return;
        }

        if (kind_ == Kind::ResetCreditExpiry) {
            remainingPercent_ = -1;
            valueText_ = FormatResetCreditDisplay(snapshot);
            return;
        }

        remainingPercent_ = kind_ == Kind::FiveHour
            ? snapshot.fiveHour.remainingPercent
            : snapshot.weekly.remainingPercent;
        valueText_ = FormatRemainingPercent(remainingPercent_);
    }

private:
    Kind kind_;
    const wchar_t* name_;
    const wchar_t* id_;
    const wchar_t* label_;
    const wchar_t* sample_;
    int remainingPercent_ = -1;
    std::wstring valueText_ = L"--";
};

class CodexUsagePlugin final : public ITMPlugin {
public:
    static CodexUsagePlugin& Instance() {
        static CodexUsagePlugin plugin;
        return plugin;
    }

    IPluginItem* GetItem(int index) override {
        if (index == 0) {
            return &fiveHourItem_;
        }
        if (index == 1) {
            return &weeklyItem_;
        }
        if (index == 2) {
            return &expiryItem_;
        }
        if (index == 3) {
            return &resetCreditItem_;
        }
        return nullptr;
    }

    void DataRequired() override {
        const auto now = std::chrono::steady_clock::now();
        CodexUsageConfig config;
        unsigned long long generation;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (fetchInProgress_) {
                return;
            }
            if (lastFetch_.time_since_epoch().count() != 0 && now - lastFetch_ < refreshInterval_) {
                return;
            }
            fetchInProgress_ = true;
            lastFetch_ = now;
            config = config_;
            generation = configGeneration_;
        }

        std::thread([this, config, generation]() {
            UsageSnapshot snapshot = fetcher_.Fetch(config);
            std::lock_guard<std::mutex> lock(mutex_);
            fetchInProgress_ = false;
            if (generation != configGeneration_) return;
            snapshot_ = std::move(snapshot);
            fiveHourItem_.SetSnapshot(snapshot_);
            weeklyItem_.SetSnapshot(snapshot_);
            expiryItem_.SetSnapshot(snapshot_);
            resetCreditItem_.SetSnapshot(snapshot_);
            tooltip_ = BuildTooltip(snapshot_);
        }).detach();
    }

    OptionReturn ShowOptionsDialog(void* parent) override {
        CodexUsageConfig config;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            config = config_;
        }
        if (!ShowCodexOptions(reinterpret_cast<HWND>(parent), &config)) return OR_OPTION_UNCHANGED;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            config_ = std::move(config);
            ++configGeneration_;
            lastFetch_ = {};
            snapshot_ = {};
            fiveHourItem_.SetSnapshot(snapshot_);
            weeklyItem_.SetSnapshot(snapshot_);
            expiryItem_.SetSnapshot(snapshot_);
            resetCreditItem_.SetSnapshot(snapshot_);
            tooltip_ = L"Codex Usage: waiting for data";
        }
        DataRequired();
        return OR_OPTION_CHANGED;
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override {
        switch (index) {
            case TMI_NAME:
                return L"Codex Usage";
            case TMI_DESCRIPTION:
                return L"Shows Codex remaining usage, subscription expiry, and reset credits.";
            case TMI_AUTHOR:
                return L"Codex";
            case TMI_COPYRIGHT:
                return L"MIT";
            case TMI_VERSION:
                return CODEX_USAGE_VERSION_WIDE;
            case TMI_URL:
                return L"https://github.com/HCLonely/TrafficMonitor_Codex_Plugin";
            default:
                return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return tooltip_.c_str();
    }

private:
    CodexUsagePlugin() = default;

    std::wstring BuildTooltip(const UsageSnapshot& snapshot) const {
        return BuildUsageTooltip(snapshot);
    }

    CodexUsageConfig config_ = LoadCodexConfig();
    unsigned long long configGeneration_ = 0;
    CodexUsageFetcher fetcher_;
    CodexUsageItem fiveHourItem_{CodexUsageItem::Kind::FiveHour, L"Codex 5h Remaining", L"CodexUsage5h", L"5\u5c0f\u65f6: ", L"100%"};
    CodexUsageItem weeklyItem_{CodexUsageItem::Kind::Weekly, L"Codex Weekly Remaining", L"CodexUsageWeek", L"\u672c \u5468: ", L"100%"};
    CodexUsageItem expiryItem_{CodexUsageItem::Kind::SubscriptionExpiry, L"Codex Plan Expires", L"CodexUsageExpires", L"\u5230\u671f: ", L"06-10 10:52"};
    CodexUsageItem resetCreditItem_{CodexUsageItem::Kind::ResetCreditExpiry, L"Codex Reset Expires", L"CodexUsageReset", L"\u91cd\u7f6e\u5361: ", L"07-18 08:42"};
    mutable std::mutex mutex_;
    UsageSnapshot snapshot_;
    std::wstring tooltip_ = L"Codex Usage: waiting for data";
    bool fetchInProgress_ = false;
    std::chrono::steady_clock::time_point lastFetch_{};
    static constexpr std::chrono::minutes refreshInterval_{1};
};

}  // namespace

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
    return &CodexUsagePlugin::Instance();
}
