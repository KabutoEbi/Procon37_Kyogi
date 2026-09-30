#include "model.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace benzen;

namespace {

MatchState makeState(const MatchSetting& setting, std::uint64_t seed) {
    MatchState state;
    state.day = 0;
    state.trafficByPos.assign(setting.field.cellCount(), TrafficStatus::Smooth);
    for (std::size_t pos = 0; pos < setting.field.cellCount(); ++pos) {
        if (setting.field.cells[pos].type == CellType::Road) {
            state.trafficByPos[pos] = static_cast<TrafficStatus>((pos + seed) % 3);
        }
    }
    for (AgentIndex a = 0; a < setting.agentCount(); ++a) {
        state.agents.push_back({
            a,
            a + 1 == setting.agentCount() ? AgentKind::Supply : AgentKind::Patrol,
            setting.agentStarts[a],
            setting.fuelLimit,
        });
    }
    return state;
}

void checkRandomMaps() {
    for (std::uint64_t seed = 1; seed <= 128; ++seed) {
        CreateConfig create;
        create.width = create.height = 8;
        create.agentCount = 3 + seed % 3;
        create.spotCount = 8;
        create.brandCount = 5;
        create.seed = seed;
        const MatchSetting setting = ProblemCreator::generate(create).setting;
        const MatchState state = makeState(setting, seed);

        for (AgentIndex a = 0; a < setting.agentCount(); ++a) {
            const CellPos start = setting.agentStarts[a];
            const auto table = brain0(setting, state, start);
            for (const Spot& spot : setting.field.spots) {
                const auto path = Brain0::restorePath(setting, table, spot.pos, AgentKind::Patrol);
                assert(path.reachable == table.canReach(spot.pos));
                if (!path.reachable)
                    continue;
                CellPos pos = start;
                int steps = 0, fuel = 0;
                for (const Direction direction : path.directions) {
                    steps += moveStepCost(setting, state, pos);
                    fuel += moveFuelCost(setting, AgentKind::Patrol, pos);
                    pos = setting.field.neighbor(pos, direction);
                    assert(setting.field.contains(pos));
                    assert(setting.field.at(pos).isPassable());
                }
                assert(pos == spot.pos);
                assert(steps == path.cost.steps && fuel == path.cost.fuel);
            }
        }

        const auto greedy = brain0Greedy(setting, state);
        const auto simulation = simulateDay(setting, state, greedy);
        assert(simulation.valid);
    }
}

MatchSetting lineSetting(int width, int steps, int fuel) {
    MatchSetting setting;
    setting.field.width = width;
    setting.field.height = 1;
    setting.field.cells.assign(static_cast<std::size_t>(width), {CellType::Plain, -1});
    setting.field.brands = {0};
    setting.agentStarts = {0};
    setting.daySteps = {steps};
    setting.daySeconds = {60};
    setting.fuelLimit = fuel;
    setting.playerCount = 2;
    assert(setting.field.rebuildSpotIndex());
    return setting;
}

int exhaustiveArrival(const MatchSetting& setting, const MatchState& state, int initialFuel,
                      int stepLimit, const std::vector<TimedRefuel>& events) {
    const int width = setting.field.width;
    std::vector<std::vector<int>> best(static_cast<std::size_t>(stepLimit + 1),
                                       std::vector<int>(static_cast<std::size_t>(width), -1));
    auto refuel = [&](int step, CellPos pos, int fuel) {
        for (const auto& event : events) {
            if (event.step == step && event.pos == pos)
                return setting.fuelLimit;
        }
        return fuel;
    };
    best[0][0] = initialFuel;
    for (int step = 0; step <= stepLimit; ++step) {
        if (best[step][width - 1] >= 0)
            return step;
        for (CellPos from = 0; from < width; ++from) {
            const int current = best[step][from];
            if (current < 0 || step == stepLimit)
                continue;
            best[step + 1][from] = std::max(best[step + 1][from], refuel(step + 1, from, current));
            const int duration = moveStepCost(setting, state, from);
            const int cost = moveFuelCost(setting, AgentKind::Patrol, from);
            if (duration > stepLimit - step)
                continue;
            for (const Direction direction : {Direction::Right, Direction::Left}) {
                const CellPos to = setting.field.neighbor(from, direction);
                if (!setting.field.contains(to))
                    continue;
                int fuel = current;
                for (int during = step + 1; during < step + duration; ++during) {
                    fuel = refuel(during, from, fuel);
                }
                if (fuel < cost)
                    continue;
                fuel -= cost;
                fuel = refuel(step + duration, to, fuel);
                best[step + duration][to] = std::max(best[step + duration][to], fuel);
            }
        }
    }
    return std::numeric_limits<int>::max();
}

void checkTimedRefuelsAgainstTimeExpansion() {
    std::mt19937 random(20261001);
    for (int test = 0; test < 256; ++test) {
        const int width = 2 + static_cast<int>(random() % 6);
        const int limit = 4 + static_cast<int>(random() % 13);
        const int fuel = 1 + static_cast<int>(random() % 4);
        auto setting = lineSetting(width, limit, fuel);
        for (CellPos pos = 0; pos < width; ++pos) {
            if (random() % 3 == 0)
                setting.field.cells[pos].type = CellType::Road;
        }
        MatchState state;
        state.day = 0;
        state.agents = {{0, AgentKind::Patrol, 0, fuel}};
        state.trafficByPos.assign(setting.field.cellCount(), TrafficStatus::Smooth);
        for (CellPos pos = 0; pos < width; ++pos) {
            if (setting.field.cells[pos].type == CellType::Road)
                state.trafficByPos[pos] = static_cast<TrafficStatus>(random() % 3);
        }
        std::vector<TimedRefuel> events;
        const int eventCount = static_cast<int>(random() % 8);
        events.reserve(static_cast<std::size_t>(eventCount));
        for (int i = 0; i < eventCount; ++i) {
            events.push_back({
                static_cast<CellPos>(random() % width),
                1 + static_cast<int>(random() % limit),
            });
        }
        const int expected = exhaustiveArrival(setting, state, fuel, limit, events);
        const auto actual = brain0TimedRefuels(setting, state, 0, width - 1, AgentKind::Patrol,
                                               fuel, fuel, 0, limit, events);
        assert(actual.reachable == (expected != std::numeric_limits<int>::max()));
        if (actual.reachable)
            assert(actual.arrivalStep == expected);
    }
}

} // namespace

int main() {
    checkRandomMaps();
    checkTimedRefuelsAgainstTimeExpansion();
}
