#pragma once

#include "agent_selector.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace benzen {

// mainの位置引数を維持し、GUIとCLIで同じ探索設定を使用する。
struct SolverOptions {
    std::vector<char*> positional;
    BeamSearchConfig beam;
    AgentSelectorConfig selection;
    bool hasTimeLimit = false;
    bool hasSelectionTimeLimit = false;
    bool autoBeamWidth = true;
    bool hasAutoBeamOption = false;
    bool hasManualBeamWidth = false;

    SolverOptions() {
        // CLIの標準60秒枠では約55秒を探索へ使う。
        beam.safetyMarginMs = 5000;
    }

private:
    static std::int64_t parseValue(
        std::string_view name, std::string_view text) {
        std::int64_t value = 0;
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size() ||
            value < 0 || value > 2147483647) {
            throw std::invalid_argument("invalid value: " + std::string(name));
        }
        return value;
    }

    void setOption(std::string_view name, std::int64_t value) {
        const bool isWidth =
            name == "--beam-width" || name == "--joint-beam-width" ||
            name == "--trial-beam-width" ||
            name == "--trial-joint-beam-width";
        if (isWidth && value == 0) {
            throw std::invalid_argument(
                "width must be positive: " + std::string(name));
        }
        if ((name == "--macro-actions" || name == "--adaptive-beam" ||
             name == "--exact-brands" || name == "--brand-repair" ||
             name == "--udon-alns" || name == "--spot-route-lns" ||
             name == "--route-columns" ||
             name == "--next-day-probe" || name == "--day-beam" || name == "--quantity-upgrades" || name == "--coordinated-refinement" ||
             name == "--small-board-brand-rescue" ||
             name == "--auto-beam-width") &&
            value > 1) {
            throw std::invalid_argument(
                "expected 0 or 1: " + std::string(name));
        }

        if (name == "--beam-width") {
            beam.beamWidth = value;
            hasManualBeamWidth = true;
        } else if (name == "--joint-beam-width") {
            beam.jointBeamWidth = value;
            hasManualBeamWidth = true;
        } else if (name == "--threads") beam.threadCount = value;
        else if (name == "--time-limit-ms") {
            beam.timeLimitMs = value;
            hasTimeLimit = true;
        } else if (name == "--safety-margin-ms") {
            beam.safetyMarginMs = value;
            selection.safetyMarginMs = value;
        } else if (name == "--max-actions") beam.maxActionsPerAgent = value;
        else if (name == "--macro-actions") beam.useMacroActions = value != 0;
        else if (name == "--adaptive-beam") beam.adaptiveBeamWidth = value != 0;
        else if (name == "--exact-brands") beam.exactBrands = value != 0;
        else if (name == "--brand-repair") beam.useBrandRepair = value != 0;
        else if (name == "--udon-alns") beam.useUdonAlns = value != 0;
        else if (name == "--spot-route-lns") beam.useSpotRouteLns = value != 0;
        else if (name == "--route-columns") beam.routeColumns.enabled = value != 0;
        else if (name == "--next-day-probe") beam.compareNextDayPlans = value != 0;
        else if (name == "--quantity-upgrades") beam.quantityUpgrades = value != 0;
        else if (name == "--coordinated-refinement") beam.coordinatedRefinement = value != 0;
        else if (name == "--supply-wait-steps") beam.maxSupplyWaitSteps = value;
        else if (name == "--small-board-brand-rescue") {
            beam.adaptiveSmallBoardBrandRescue = value != 0;
        } else if (name == "--small-board-beam-budget-percent") {
            if (value < 50 || value > 95)
                throw std::invalid_argument("small-board beam budget must be 50..95");
            beam.smallBoardBeamBudgetPercent = value;
        } else if (name == "--small-board-brand-node-limit") {
            if (value < 1000)
                throw std::invalid_argument("small-board brand node limit must be at least 1000");
            beam.smallBoardBrandSearchNodeLimit = value;
        }
        else if (name == "--day-beam") {
            beam.useDayBeam = value != 0;
            if (value) beam.quantityUpgrades = true;
        }
        else if (name == "--day-beam-width") {
            if (value < 2 || value > 8) throw std::invalid_argument("day beam width must be 2..8");
            beam.dayBeamWidth = value;
        } else if (name == "--day-beam-depth") {
            if (value < 1 || value > 3) throw std::invalid_argument("day beam depth must be 1..3");
            beam.dayBeamDepth = value;
        } else if (name == "--day-beam-max-udon-loss") {
            beam.dayBeamMaxUdonLoss = value;
        } else if (name == "--quantity-budget-percent") {
            if (value < 1 || value > 90) throw std::invalid_argument("quantity budget must be 1..90");
            beam.quantityBudgetPercent = value;
        }
        else if (name == "--auto-beam-width") {
            autoBeamWidth = value != 0;
            hasAutoBeamOption = true;
        } else if (name == "--trial-candidates") selection.trialCandidateLimit = value;
        else if (name == "--trial-beam-width") selection.trialBeamWidth = value;
        else if (name == "--trial-joint-beam-width") {
            selection.trialJointBeamWidth = value;
        } else if (name == "--selection-time-ms") {
            selection.timeLimitMs = value;
            hasSelectionTimeLimit = true;
        } else {
            throw std::invalid_argument("unknown option: " + std::string(name));
        }
    }

public:
    static SolverOptions parse(int argc, char** argv) {
        SolverOptions options;
        options.positional.push_back(argv[0]);
        for (int i = 1; i < argc; ++i) {
            const std::string_view name(argv[i]);
            if (name.size() < 2 || name[0] != '-' || name[1] != '-') {
                options.positional.push_back(argv[i]);
                continue;
            }
            if (++i == argc) {
                throw std::invalid_argument(
                    "missing value: " + std::string(name));
            }
            options.setOption(name, parseValue(name, argv[i]));
        }
        if (!options.hasAutoBeamOption && options.hasManualBeamWidth)
            options.autoBeamWidth = false;
        return options;
    }

    BeamSearchConfig beamFor(
        const MatchSetting& setting, const MatchState& state,
        BeamSearchConfig config) const {
        // An explicit zero keeps the established unlimited-search contract.
        if (!autoBeamWidth ||
            ((hasTimeLimit || config.exactBrands) && config.timeLimitMs == 0))
            return config;

        // Preserve a hard time bound even for offline states without endsAt.
        // 60秒枠の5秒前までを標準探索予算とする。
        const std::int64_t targetMs = 55000;
        const std::int64_t cappedLimit = targetMs + config.safetyMarginMs;
        if (config.timeLimitMs == 0 || config.timeLimitMs > cappedLimit)
            config.timeLimitMs = cappedLimit;
        const auto effectiveMs = std::max<std::int64_t>(1,
            config.timeLimitMs - config.safetyMarginMs);

        const double size = static_cast<double>(std::max(
            setting.field.width, setting.field.height));
        double base = 44000.0;
        double referenceAgents = 4.0;
        double ceiling = 44000.0;
        if (size > 16.0 && size <= 24.0) {
            const double share = (size - 16.0) / 8.0;
            base = 44000.0 * std::pow(14300.0 / 44000.0, share);
            referenceAgents = 4.0 + share;
            ceiling = 33000.0;
        } else if (size > 24.0 && size <= 32.0) {
            const double share = (size - 24.0) / 8.0;
            base = 14300.0 * std::pow(5940.0 / 14300.0, share);
            referenceAgents = 5.0 + 3.0 * share;
            ceiling = 13200.0;
        } else if (size > 32.0) {
            base = 5940.0 * 32.0 * 32.0 /
                std::max(1.0, static_cast<double>(setting.field.cellCount()));
            referenceAgents = 8.0;
            ceiling = 13200.0;
        }

        const auto agents = std::max<std::size_t>(1, state.agents.size());
        const double agentFactor = std::clamp(
            referenceAgents / static_cast<double>(agents), 0.5, 1.5);
        const auto day = state.day >= 0 ?
            static_cast<std::size_t>(state.day) : std::size_t{0};
        double dayFactor = 1.0;
        if (day < setting.daySteps.size() && !setting.daySteps.empty() &&
            setting.daySteps.front() > 0 && setting.daySteps[day] > 0) {
            const double stepRatio = static_cast<double>(setting.daySteps.front()) /
                static_cast<double>(setting.daySteps[day]);
            if (size <= 16.0) {
                dayFactor = std::pow(stepRatio, 0.75);
            } else {
                const double shorterDay = std::max(1.0, stepRatio);
                dayFactor = std::pow(shorterDay, 1.7);
                if (shorterDay > 1.0)
                    dayFactor *= 1.0 + 0.1 * static_cast<double>(
                        std::min<std::size_t>(day, 3));
            }
        }
        const double timeFactor = effectiveMs >= targetMs ? 1.0 :
            std::clamp(static_cast<double>(effectiveMs) / targetMs, 0.01, 1.0);
        const double desired = base * agentFactor * dayFactor * timeFactor;
        config.beamWidth = static_cast<std::size_t>(std::clamp(
            std::round(desired), 128.0, ceiling));
        config.jointBeamWidth = config.beamWidth * 4;
        config.autoSizedBeam = true;
        // The size-based adaptive multiplier would apply a second reduction
        // to these already calibrated widths.
        config.adaptiveBeamWidth = false;
        return config;
    }

    AgentSelectorConfig selectorFor(const MatchSetting& setting) const {
        auto config = selection;
        config.extendTrialsToDeadline = true;
        if (!hasSelectionTimeLimit) {
            const auto size = std::max(setting.field.width, setting.field.height);
            config.timeLimitMs = size <= 16 ? 60000 : (size <= 24 ? 90000 : 120000);
        }
        // 種別選択も試合開始時刻を超えて試走しない。
        config.deadlineUnixSeconds = setting.startsAt;
        return config;
    }
};

}  // namespace benzen
