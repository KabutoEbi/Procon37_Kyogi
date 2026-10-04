#include "solver_options.hpp"
#include <cassert>
#include <string>
#include <vector>

benzen::SolverOptions parse(std::vector<std::string> values) {
    std::vector<char*> args;
    for (auto& value : values) args.push_back(value.data());
    auto result = benzen::SolverOptions::parse(static_cast<int>(args.size()), args.data());
    result.positional.clear(); // このテストでは設定値だけを検査する。
    return result;
}

int main() {
    const auto options = parse({"main", "solve", "setting.json", "state.json",
        "--beam-width", "17", "--joint-beam-width", "53", "--threads", "2",
        "--time-limit-ms", "300", "--safety-margin-ms", "25",
        "--macro-actions", "0", "--adaptive-beam", "0",
        "--trial-candidates", "0", "--selection-time-ms", "100"});
    assert(options.beam.beamWidth == 17 && options.beam.jointBeamWidth == 53);
    assert(options.beam.threadCount == 2 && options.beam.timeLimitMs == 300);
    assert(!options.beam.useMacroActions && !options.beam.adaptiveBeamWidth);
    assert(options.hasTimeLimit && options.hasSelectionTimeLimit);
    assert(options.selection.trialCandidateLimit == 0);
    assert(options.selection.minSupplyCount == 1);
    benzen::MatchSetting setting;
    setting.startsAt = 1800000000;
    assert(options.selectorFor(setting).deadlineUnixSeconds == setting.startsAt);
    assert(options.selectorFor(setting).timeLimitMs == 100);
    assert(options.selectorFor(setting).extendTrialsToDeadline);
    assert(parse({"main", "--exact-brands", "1"}).beam.exactBrands);
    assert(!parse({"main", "--brand-repair", "0"}).beam.useBrandRepair);
    assert(parse({"main", "--brand-repair", "1"}).beam.useBrandRepair);
    assert(!parse({"main", "--udon-alns", "0"}).beam.useUdonAlns);
    assert(parse({"main", "--udon-alns", "1"}).beam.useUdonAlns);
    assert(!parse({"main", "--spot-route-lns", "0"}).beam.useSpotRouteLns);
    for (const auto& invalid : std::vector<std::vector<std::string>>{
        {"main", "--beam-width", "0"}, {"main", "--threads", "-1"},
        {"main", "--time-limit-ms", "3ms"}, {"main", "--unknown", "1"},
        {"main", "--beam-width"}, {"main", "--macro-actions", "2"},
        {"main", "--exact-brands", "2"}, {"main", "--brand-repair", "2"},
        {"main", "--udon-alns", "2"}, {"main", "--spot-route-lns", "2"}}) {
        bool rejected = false;
        try { (void)parse(invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
}
