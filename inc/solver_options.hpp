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

    static SolverOptions parse(int argc, char** argv) {
        SolverOptions out;
        out.positional.push_back(argv[0]);
        for (int i = 1; i < argc; ++i) {
            const std::string_view name(argv[i]);
            if (name.substr(0, 2) != "--") {
                out.positional.push_back(argv[i]);
                continue;
            }
            if (++i == argc) throw std::invalid_argument("missing value: " + std::string(name));
            const std::string_view text(argv[i]);
            std::int64_t value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                value < 0 || value > 2147483647) {
                throw std::invalid_argument("invalid value: " + std::string(name));
            }
            if ((name == "--beam-width" || name == "--joint-beam-width" ||
                 name == "--trial-beam-width" || name == "--trial-joint-beam-width") && value == 0)
                throw std::invalid_argument("width must be positive: " + std::string(name));
            if ((name == "--macro-actions" || name == "--adaptive-beam") && value > 1)
                throw std::invalid_argument("expected 0 or 1: " + std::string(name));
            if (name == "--beam-width") out.beam.beamWidth = value;
            else if (name == "--joint-beam-width") out.beam.jointBeamWidth = value;
            else if (name == "--threads") out.beam.threadCount = value;
            else if (name == "--time-limit-ms") {
                out.beam.timeLimitMs = value;
                out.hasTimeLimit = true;
            } else if (name == "--safety-margin-ms") {
                out.beam.safetyMarginMs = value;
                out.selection.safetyMarginMs = value;
            } else if (name == "--max-actions") out.beam.maxActionsPerAgent = value;
            else if (name == "--macro-actions") out.beam.useMacroActions = value != 0;
            else if (name == "--adaptive-beam") out.beam.adaptiveBeamWidth = value != 0;
            else if (name == "--trial-candidates") out.selection.trialCandidateLimit = value;
            else if (name == "--trial-beam-width") out.selection.trialBeamWidth = value;
            else if (name == "--trial-joint-beam-width") out.selection.trialJointBeamWidth = value;
            else if (name == "--selection-time-ms") {
                out.selection.timeLimitMs = value;
                out.hasSelectionTimeLimit = true;
            } else throw std::invalid_argument("unknown option: " + std::string(name));
        }
        return out;
    }

    AgentSelectorConfig selectorFor(const MatchSetting& setting) const {
        auto config = selection;
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
