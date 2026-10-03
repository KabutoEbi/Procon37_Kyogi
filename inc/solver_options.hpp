#pragma once

#include "agent_selector.hpp"
#include <algorithm>
#include <charconv>
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
             name == "--udon-alns") &&
            value > 1) {
            throw std::invalid_argument(
                "expected 0 or 1: " + std::string(name));
        }

        if (name == "--beam-width") beam.beamWidth = value;
        else if (name == "--joint-beam-width") beam.jointBeamWidth = value;
        else if (name == "--threads") beam.threadCount = value;
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
        else if (name == "--trial-candidates") selection.trialCandidateLimit = value;
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
        return options;
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
