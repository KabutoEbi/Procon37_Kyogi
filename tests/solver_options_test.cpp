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
    assert(parse({"main"}).selection.trialCandidateLimit == 6);
    assert(parse({"main"}).beam.safetyMarginMs == 5000);
    assert(parse({"main"}).beam.coordinatedRefinement);
    assert(!parse({"main", "--coordinated-refinement", "0"}).beam.coordinatedRefinement);
    assert(parse({"main", "--supply-wait-steps", "5"})
        .beam.maxSupplyWaitSteps == 5);
    assert(parse({"main"}).beam.adaptiveSmallBoardBrandRescue);
    const auto smallRescue = parse({"main", "--small-board-brand-rescue", "0",
        "--small-board-beam-budget-percent", "80",
        "--small-board-brand-node-limit", "150000"});
    assert(!smallRescue.beam.adaptiveSmallBoardBrandRescue);
    assert(smallRescue.beam.smallBoardBeamBudgetPercent == 80);
    assert(smallRescue.beam.smallBoardBrandSearchNodeLimit == 150000);
    assert(!parse({"main"}).beam.quantityUpgrades);
    assert(parse({"main"}).beam.adaptiveLongSearch);
    assert(parse({"main"}).beam.udonAlns.simulatedAnnealing);
    assert(parse({"main"}).beam.udonAlns.jointDestroyRepair);
    assert(!parse({"main", "--udon-sa", "0"})
        .beam.udonAlns.simulatedAnnealing);
    assert(!parse({"main", "--udon-joint-destroy", "0"})
        .beam.udonAlns.jointDestroyRepair);
    assert(!parse({"main", "--adaptive-long-search", "0"})
        .beam.adaptiveLongSearch);
    assert(parse({"main", "--quantity-upgrades", "1"}).beam.quantityUpgrades);
    assert(parse({"main", "--day-beam", "1"}).beam.quantityUpgrades);
    const auto quantity = parse({"main", "--day-beam", "0", "--day-beam-width", "6",
        "--day-beam-depth", "3", "--quantity-budget-percent", "30",
        "--day-beam-max-udon-loss", "0"});
    assert(!quantity.beam.useDayBeam && quantity.beam.dayBeamWidth == 6);
    assert(quantity.beam.dayBeamDepth == 3 && quantity.beam.quantityBudgetPercent == 30);
    assert(!quantity.beam.adaptiveLongSearch);
    assert(!parse({"main", "--quantity-budget-percent", "30",
        "--adaptive-long-search", "1"}).beam.adaptiveLongSearch);
    assert(quantity.beam.dayBeamMaxUdonLoss == 0);
    for (const auto& args : std::vector<std::vector<std::string>>{
        {"main", "--coordinated-refinement", "2"},
        {"main", "--adaptive-long-search", "2"},
        {"main", "--udon-sa", "2"},
        {"main", "--udon-joint-destroy", "2"},
        {"main", "--small-board-brand-rescue", "2"},
        {"main", "--small-board-beam-budget-percent", "49"},
        {"main", "--small-board-beam-budget-percent", "96"},
        {"main", "--small-board-brand-node-limit", "999"},
        {"main", "--day-beam", "2"}, {"main", "--day-beam-width", "1"},
        {"main", "--day-beam-depth", "4"}, {"main", "--quantity-budget-percent", "91"}}) {
        bool rejected = false;
        try { (void)parse(args); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
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
    assert(!options.autoBeamWidth);
    assert(parse({"main"}).autoBeamWidth);
    assert(parse({"main", "--beam-width", "17", "--auto-beam-width", "1"})
        .autoBeamWidth);
    assert(!parse({"main", "--auto-beam-width", "0"}).autoBeamWidth);
    setting.field.width = 24;
    setting.field.height = 24;
    setting.daySteps = {110, 142, 73, 82};
    benzen::MatchState state;
    state.agents.resize(5);
    auto automatic = parse({"main"});
    auto autoConfig = automatic.beam;
    autoConfig.timeLimitMs = 60000;
    autoConfig.safetyMarginMs = 5000;
    const auto first = automatic.beamFor(setting, state, autoConfig);
    assert(first.beamWidth == 14300 && first.jointBeamWidth == 57200);
    assert(!first.adaptiveBeamWidth && first.timeLimitMs == 60000);
    state.day = 1;
    const auto second = automatic.beamFor(setting, state, autoConfig);
    state.day = 2;
    const auto third = automatic.beamFor(setting, state, autoConfig);
    assert(second.beamWidth == first.beamWidth);
    assert(third.beamWidth > second.beamWidth);
    autoConfig.timeLimitMs = 1000;
    const auto shortRun = automatic.beamFor(setting, state, autoConfig);
    assert(shortRun.beamWidth < third.beamWidth);
    autoConfig.timeLimitMs = 90000;
    assert(automatic.beamFor(setting, state, autoConfig).timeLimitMs == 60000);
    assert(options.beamFor(setting, state, options.beam).beamWidth == 17);
    const auto unlimited = parse({"main", "--time-limit-ms", "0"});
    assert(unlimited.beamFor(setting, state, unlimited.beam).timeLimitMs == 0);
    assert(unlimited.beamFor(setting, state, unlimited.beam).beamWidth == 128);
    const auto exact = parse({"main", "--exact-brands", "1"});
    assert(exact.beamFor(setting, state, exact.beam).timeLimitMs == 0);
    setting.field.width = 16;
    setting.field.height = 16;
    setting.daySteps = {32, 48};
    state.day = 0;
    state.agents.resize(4);
    assert(automatic.beamFor(setting, state, autoConfig).beamWidth == 44000);
    state.day = 1;
    assert(automatic.beamFor(setting, state, autoConfig).beamWidth < 44000);
    setting.field.width = 32;
    setting.field.height = 32;
    setting.daySteps = {64};
    state.day = 0;
    state.agents.resize(8);
    assert(automatic.beamFor(setting, state, autoConfig).beamWidth == 5940);
    assert(parse({"main", "--exact-brands", "1"}).beam.exactBrands);
    assert(!parse({"main", "--brand-repair", "0"}).beam.useBrandRepair);
    assert(parse({"main", "--brand-repair", "1"}).beam.useBrandRepair);
    assert(!parse({"main", "--udon-alns", "0"}).beam.useUdonAlns);
    assert(parse({"main", "--udon-alns", "1"}).beam.useUdonAlns);
    assert(!parse({"main", "--spot-route-lns", "0"}).beam.useSpotRouteLns);
    assert(!parse({"main", "--route-columns", "0"}).beam.routeColumns.enabled);
    assert(parse({"main", "--route-columns", "1"}).beam.routeColumns.enabled);
    assert(!parse({"main", "--next-day-probe", "0"}).beam.compareNextDayPlans);
    assert(parse({"main", "--next-day-probe", "1"}).beam.compareNextDayPlans);
    for (const auto& invalid : std::vector<std::vector<std::string>>{
        {"main", "--beam-width", "0"}, {"main", "--threads", "-1"},
        {"main", "--time-limit-ms", "3ms"}, {"main", "--unknown", "1"},
        {"main", "--beam-width"}, {"main", "--macro-actions", "2"},
        {"main", "--exact-brands", "2"}, {"main", "--brand-repair", "2"},
        {"main", "--udon-alns", "2"}, {"main", "--spot-route-lns", "2"},
        {"main", "--route-columns", "2"},
        {"main", "--adaptive-long-search", "2"},
        {"main", "--next-day-probe", "2"},
        {"main", "--auto-beam-width", "2"}}) {
        bool rejected = false;
        try { (void)parse(invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
}
