#pragma once

#include "game.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <queue>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace benzen {

struct CreateTeam {
    std::string name;
    std::string token;
};

struct CreateConfig {
    std::int32_t width = 16;
    std::int32_t height = 16;
    std::int32_t agentCount = 4;
    std::int32_t spotCount = 16;
    std::int32_t brandCount = 12;
    std::int32_t dayCount = 4;
    std::int32_t daySeconds = 60;
    std::int32_t teamCount = 2;

    // Plain, Road, Mountain, Pond の生成比率。
    std::array<double, 4> terrainWeights = {45.0, 30.0, 20.0, 5.0};

    // 同じseedなら同じ問題を生成する。
    std::uint64_t seed = 1;
    std::int32_t maxGenerationAttempts = 10000;
};

struct GeneratedProblem {
    MatchSetting setting;
    std::vector<CreateTeam> teams;
    std::uint64_t seed = 0;
};

struct CreateMatchStateConfig {
    std::int32_t day = 0;
    std::int64_t endsAt = 0;
    std::int32_t supplyCount = 1;
    std::uint64_t seed = 1;
};

class ProblemCreator final {
private:
    static void validateConfig(const CreateConfig& config) {
        if (config.width < 8 || config.width > 32 ||
            config.height < 8 || config.height > 32) {
            throw std::invalid_argument("width and height must be in [8, 32]");
        }
        if (config.agentCount < 3 || config.agentCount > 8) {
            throw std::invalid_argument("agentCount must be in [3, 8]");
        }
        if (config.spotCount < config.agentCount ||
            config.spotCount > std::max(config.width, config.height) ||
            config.spotCount > static_cast<std::int32_t>(kMaxSpots)) {
            throw std::invalid_argument(
                "spotCount must be in [agentCount, max(width, height)]");
        }
        if (config.brandCount < 1 ||
            config.brandCount > config.spotCount ||
            config.brandCount > static_cast<std::int32_t>(kMaxBrands)) {
            throw std::invalid_argument("brandCount must be in [1, spotCount]");
        }
        if (config.dayCount < 4 || config.dayCount > 10) {
            throw std::invalid_argument("dayCount must be in [4, 10]");
        }
        if (config.daySeconds <= 0) {
            throw std::invalid_argument("daySeconds must be positive");
        }
        if (config.teamCount <= 0) {
            throw std::invalid_argument("teamCount must be positive");
        }
        if (config.maxGenerationAttempts <= 0) {
            throw std::invalid_argument(
                "maxGenerationAttempts must be positive");
        }
        for (const double weight : config.terrainWeights) {
            if (weight <= 0.0) {
                throw std::invalid_argument(
                    "every terrain weight must be positive");
            }
        }
    }

    static bool allTerrainTypesExist(const Field& field) noexcept {
        std::array<bool, 4> found{};
        for (const Cell& cell : field.cells) {
            const std::size_t type = static_cast<std::size_t>(cell.type);
            if (type < found.size()) {
                found[type] = true;
            }
        }
        return std::all_of(found.begin(), found.end(), [](bool value) {
            return value;
        });
    }

    static bool passableCellsAreConnected(const Field& field) {
        CellPos start = kInvalidCell;
        std::size_t passableCount = 0;
        for (std::size_t pos = 0; pos < field.cells.size(); ++pos) {
            if (field.cells[pos].isPassable()) {
                ++passableCount;
                if (start == kInvalidCell) {
                    start = static_cast<CellPos>(pos);
                }
            }
        }
        if (start == kInvalidCell) {
            return false;
        }

        std::vector<bool> visited(field.cellCount(), false);
        std::queue<CellPos> queue;
        visited[static_cast<std::size_t>(start)] = true;
        queue.push(start);
        std::size_t reached = 0;
        while (!queue.empty()) {
            const CellPos current = queue.front();
            queue.pop();
            ++reached;
            for (const CellPos next : field.neighbors(current)) {
                if (!field.contains(next) || !field.at(next).isPassable()) {
                    continue;
                }
                const std::size_t index = static_cast<std::size_t>(next);
                if (!visited[index]) {
                    visited[index] = true;
                    queue.push(next);
                }
            }
        }
        return reached == passableCount;
    }

    static Field generateFieldImpl(
        const CreateConfig& config,
        std::mt19937_64& random) {
        std::discrete_distribution<std::int32_t> terrainDistribution(
            config.terrainWeights.begin(), config.terrainWeights.end());

        Field field;
        field.width = config.width;
        field.height = config.height;
        field.cells.resize(field.cellCount());

        for (std::int32_t attempt = 0;
             attempt < config.maxGenerationAttempts;
             ++attempt) {
            std::size_t plainCount = 0;
            for (std::size_t pos = 0; pos < field.cellCount(); ++pos) {
                const auto type = static_cast<CellType>(
                    terrainDistribution(random));
                field.cells[pos] = Cell{type, -1};
                plainCount += type == CellType::Plain ? 1U : 0U;
            }

            if (plainCount < static_cast<std::size_t>(
                    config.agentCount + config.spotCount) ||
                !allTerrainTypesExist(field) ||
                !passableCellsAreConnected(field)) {
                continue;
            }
            return field;
        }
        throw std::runtime_error(
            "failed to generate a connected field; adjust terrain weights");
    }

    static std::string escapeJson(std::string_view value) {
        static constexpr char hex[] = "0123456789abcdef";
        std::string escaped;
        escaped.reserve(value.size() + 2);
        for (const unsigned char character : value) {
            switch (character) {
                case '"':
                    escaped += "\\\"";
                    break;
                case '\\':
                    escaped += "\\\\";
                    break;
                case '\b':
                    escaped += "\\b";
                    break;
                case '\f':
                    escaped += "\\f";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    if (character < 0x20U) {
                        escaped += "\\u00";
                        escaped += hex[(character >> 4U) & 0x0fU];
                        escaped += hex[character & 0x0fU];
                    } else {
                        escaped += static_cast<char>(character);
                    }
                    break;
            }
        }
        return escaped;
    }

    template <class Values>
    static void writeArray(
        std::ostringstream& out,
        std::string_view indent,
        std::string_view name,
        const Values& vals,
        bool comma = true) {
        out << indent << '"' << name << "\": [";
        for (std::size_t i = 0; i < vals.size(); ++i) {
            out << (i == 0 ? "" : ", ") << vals[i];
        }
        out << ']' << (comma ? ",\n" : "\n");
    }

    static void writeCells(
        std::ostringstream& out,
        std::string_view indent,
        const Field& f,
        bool comma = false) {
        out << indent << "\"cells\": [\n";
        for (std::int32_t y = 0; y < f.height; ++y) {
            out << indent << "  [";
            for (std::int32_t x = 0; x < f.width; ++x) {
                const CellPos pos = y * f.width + x;
                out << (x == 0 ? "" : ", ")
                    << static_cast<std::int32_t>(f.at(pos).type);
            }
            out << ']' << (y + 1 == f.height ? "\n" : ",\n");
        }
        out << indent << ']' << (comma ? ",\n" : "\n");
    }

    static void writeSpots(
        std::ostringstream& out,
        std::string_view indent,
        const Field& f,
        bool comma = true) {
        out << indent << "\"spots\": [\n";
        for (std::size_t i = 0; i < f.spots.size(); ++i) {
            const Spot& s = f.spots[i];
            out << indent << "  {\"brand\": " << s.brand
                << ", \"pos\": " << s.pos
                << ", \"stocks\": " << s.maxStock << '}'
                << (i + 1 == f.spots.size() ? "\n" : ",\n");
        }
        out << indent << ']' << (comma ? ",\n" : "\n");
    }

    static void writeAgents(
        std::ostringstream& out,
        const std::vector<Agent>& agents,
        std::string_view indent) {
        out << "[\n";
        for (std::size_t i = 0; i < agents.size(); ++i) {
            const Agent& a = agents[i];
            out << indent << "{\"kind\": "
                << static_cast<std::int32_t>(a.kind)
                << ", \"pos\": " << a.pos
                << ", \"fuel\": " << a.fuel << '}'
                << (i + 1 == agents.size() ? "\n" : ",\n");
        }
        out << std::string(indent.size() >= 2 ? indent.size() - 2 : 0, ' ')
            << ']';
    }

    static bool writeFile(
        const std::string& path,
        const std::string& text) {
        std::ofstream out(path);
        return out && static_cast<bool>(out << text);
    }

public:
    // 地形だけを生成する。スポットと初期位置はまだ配置しない。
    static Field generateField(const CreateConfig& config = {}) {
        validateConfig(config);
        std::mt19937_64 random(config.seed);
        return generateFieldImpl(config, random);
    }

    static GeneratedProblem generate(const CreateConfig& config = {}) {
        validateConfig(config);
        std::mt19937_64 random(config.seed);
        Field field = generateFieldImpl(config, random);
        std::vector<CellPos> plainCells;
        plainCells.reserve(field.cellCount());
        for (std::size_t pos = 0; pos < field.cellCount(); ++pos) {
            if (field.cells[pos].type == CellType::Plain) {
                plainCells.push_back(static_cast<CellPos>(pos));
            }
        }

        std::shuffle(plainCells.begin(), plainCells.end(), random);

        std::vector<std::int32_t> brandAssignments(
            static_cast<std::size_t>(config.spotCount));
        for (std::int32_t i = 0; i < config.brandCount; ++i) {
            brandAssignments[static_cast<std::size_t>(i)] = i;
        }
        std::uniform_int_distribution<std::int32_t> brandDistribution(
            0, config.brandCount - 1);
        for (std::int32_t i = config.brandCount;
             i < config.spotCount;
             ++i) {
            brandAssignments[static_cast<std::size_t>(i)] =
                brandDistribution(random);
        }
        std::shuffle(
            brandAssignments.begin(), brandAssignments.end(), random);

        std::uniform_int_distribution<std::int32_t> stockDistribution(
            1, config.agentCount);
        field.spots.reserve(static_cast<std::size_t>(config.spotCount));
        for (std::int32_t i = 0; i < config.spotCount; ++i) {
            field.spots.push_back(Spot{
                brandAssignments[static_cast<std::size_t>(i)],
                plainCells[static_cast<std::size_t>(i)],
                stockDistribution(random),
                -1,
            });
        }
        if (!field.rebuildSpotIndex()) {
            throw std::runtime_error("generated spot configuration is invalid");
        }

        MatchSetting setting;
        setting.field = std::move(field);
        setting.agentStarts.reserve(
            static_cast<std::size_t>(config.agentCount));
        for (std::int32_t i = 0; i < config.agentCount; ++i) {
            setting.agentStarts.push_back(
                plainCells[static_cast<std::size_t>(config.spotCount + i)]);
        }

        const std::int32_t baseSteps = config.width + config.height;
        std::uniform_int_distribution<std::int32_t> stepDistribution(
            baseSteps, baseSteps * 4);
        setting.daySteps.reserve(static_cast<std::size_t>(config.dayCount));
        setting.daySeconds.assign(
            static_cast<std::size_t>(config.dayCount), config.daySeconds);
        for (std::int32_t day = 0; day < config.dayCount; ++day) {
            setting.daySteps.push_back(stepDistribution(random));
        }

        std::uniform_int_distribution<std::int32_t> fuelDistribution(
            setting.daySteps.front(), setting.daySteps.front() * 3);
        setting.fuelLimit = fuelDistribution(random);
        setting.playerCount = config.teamCount;
        setting.busyThreshold =
            std::uniform_int_distribution<std::int32_t>(1, 5)(random);
        setting.jammedThreshold =
            std::uniform_int_distribution<std::int32_t>(
                std::max(2, setting.busyThreshold + 1), 10)(random);

        GeneratedProblem problem;
        problem.setting = std::move(setting);
        problem.seed = config.seed;
        problem.teams.reserve(static_cast<std::size_t>(config.teamCount));
        for (std::int32_t i = 0; i < config.teamCount; ++i) {
            problem.teams.push_back(CreateTeam{
                "team-" + std::to_string(i),
                "token-p" + std::to_string(i),
            });
        }
        return problem;
    }

    // 簡易サーバーの -config にそのまま渡せるJSONを生成する。
    static std::string toJson(const GeneratedProblem& problem) {
        const MatchSetting& s = problem.setting;
        const Field& f = s.field;
        std::ostringstream out;
        out << "{\n  \"problem\": {\n";
        writeSpots(out, "    ", f);
        writeArray(out, "    ", "agentStarts", s.agentStarts);
        out << "    \"fuelLimits\": " << s.fuelLimit << ",\n";
        writeArray(out, "    ", "daySteps", s.daySteps);
        writeArray(out, "    ", "daySeconds", s.daySeconds);
        out << "    \"busyThreshold\": " << s.busyThreshold << ",\n"
            << "    \"jammedThreshold\": " << s.jammedThreshold << ",\n"
            << "    \"width\": " << f.width << ",\n"
            << "    \"height\": " << f.height << ",\n";
        writeCells(out, "    ", f);
        out << "  },\n  \"teams\": [\n";
        for (std::size_t i = 0; i < problem.teams.size(); ++i) {
            const CreateTeam& t = problem.teams[i];
            out << "    {";
            if (!t.name.empty()) {
                out << "\"name\": \"" << escapeJson(t.name) << "\", ";
            }
            out << "\"token\": \"" << escapeJson(t.token) << "\"}"
                << (i + 1 == problem.teams.size() ? "\n" : ",\n");
        }
        out << "  ]\n}\n";
        return out.str();
    }

    static bool writeJson(
        const GeneratedProblem& problem,
        const std::string& path) {
        return writeFile(path, toJson(problem));
    }

    // GET /setting と同じ「試合開始前のマップ構成フォーマット」。
    static std::string toMatchSettingJson(const MatchSetting& setting) {
        const Field& f = setting.field;
        std::ostringstream out;
        out << "{\n  \"startsAt\": " << setting.startsAt << ",\n";
        writeArray(out, "  ", "daySeconds", setting.daySeconds);
        writeArray(out, "  ", "daySteps", setting.daySteps);
        out << "  \"map\": {\n"
            << "    \"height\": " << f.height << ",\n"
            << "    \"width\": " << f.width << ",\n";
        writeCells(out, "    ", f);
        out << "  },\n";
        writeSpots(out, "  ", f);
        writeArray(out, "  ", "agents", setting.agentStarts);
        out << "  \"fuelLimits\": " << setting.fuelLimit << ",\n"
            << "  \"players\": " << setting.playerCount << ",\n"
            << "  \"busyThreshold\": " << setting.busyThreshold << ",\n"
            << "  \"jammedThreshold\": " << setting.jammedThreshold << '\n'
            << "}\n";
        return out.str();
    }

    static bool writeMatchSettingJson(
        const MatchSetting& setting,
        const std::string& path) {
        return writeFile(path, toMatchSettingJson(setting));
    }

    // テスト用の各日開始状態を生成する。day=0では公式どおり初期位置、
    // 満タン、全道路順調とし、day>0では有効範囲内でランダム化する。
    static MatchState generateMatchState(
        const MatchSetting& setting,
        const CreateMatchStateConfig& config = {}) {
        if (config.day < 0 ||
            static_cast<std::size_t>(config.day) >= setting.dayCount()) {
            throw std::invalid_argument("day is outside the match range");
        }
        if (config.supplyCount < 0 ||
            config.supplyCount >=
                static_cast<std::int32_t>(setting.agentCount())) {
            throw std::invalid_argument(
                "supplyCount must be in [0, agentCount - 1]");
        }
        if (setting.playerCount <= 0 ||
            setting.agentStarts.empty() ||
            setting.field.cells.size() != setting.field.cellCount()) {
            throw std::invalid_argument("match setting is incomplete");
        }

        std::mt19937_64 random(config.seed);
        std::vector<CellPos> passable;
        passable.reserve(setting.field.cellCount());
        for (std::size_t pos = 0;
             pos < setting.field.cellCount();
             ++pos) {
            if (setting.field.cells[pos].isPassable()) {
                passable.push_back(static_cast<CellPos>(pos));
            }
        }
        if (passable.empty()) {
            throw std::invalid_argument("field has no passable cell");
        }

        const std::size_t firstSupply =
            setting.agentCount() -
            static_cast<std::size_t>(config.supplyCount);
        auto generateAgents = [&]() {
            std::vector<Agent> agents;
            agents.reserve(setting.agentCount());
            for (std::size_t i = 0; i < setting.agentCount(); ++i) {
                const AgentKind kind = i >= firstSupply
                                           ? AgentKind::Supply
                                           : AgentKind::Patrol;
                CellPos pos = setting.agentStarts[i];
                std::int32_t fuel = setting.fuelLimit;
                if (config.day > 0) {
                    pos = passable[std::uniform_int_distribution<std::size_t>(
                        0, passable.size() - 1)(random)];
                    if (kind == AgentKind::Patrol) {
                        fuel = std::uniform_int_distribution<std::int32_t>(
                            0, setting.fuelLimit)(random);
                    }
                }
                agents.push_back(Agent{i, kind, pos, fuel});
            }
            return agents;
        };

        MatchState state;
        state.endsAt = config.endsAt;
        state.day = config.day;
        state.agents = generateAgents();
        for (std::int32_t player = 1;
             player < setting.playerCount;
             ++player) {
            state.others.push_back(TeamState{
                player,
                generateAgents(),
            });
        }

        state.trafficByPos.assign(
            setting.field.cellCount(), TrafficStatus::Smooth);
        if (config.day > 0) {
            std::uniform_int_distribution<std::int32_t> trafficDistribution(
                0, 2);
            for (std::size_t pos = 0;
                 pos < setting.field.cellCount();
                 ++pos) {
                if (setting.field.cells[pos].type == CellType::Road) {
                    state.trafficByPos[pos] = static_cast<TrafficStatus>(
                        trafficDistribution(random));
                }
            }
        }
        return state;
    }

    // GET / と同じ「各日開始時の試合情報フォーマット」。
    static std::string toMatchStateJson(
        const MatchState& state,
        const Field& field) {
        std::ostringstream out;
        out << "{\n"
            << "  \"endsAt\": " << state.endsAt << ",\n"
            << "  \"day\": " << state.day << ",\n"
            << "  \"agents\": ";
        writeAgents(out, state.agents, "    ");
        out << ",\n  \"others\": [\n";
        for (std::size_t i = 0; i < state.others.size(); ++i) {
            const TeamState& t = state.others[i];
            out << "    {\n      \"id\": " << t.id
                << ",\n      \"agents\": ";
            writeAgents(out, t.agents, "        ");
            out << "\n    }"
                << (i + 1 == state.others.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"traffics\": [\n";
        bool first = true;
        for (std::size_t pos = 0; pos < field.cellCount(); ++pos) {
            if (field.cells[pos].type != CellType::Road) {
                continue;
            }
            if (!first) {
                out << ",\n";
            }
            const TrafficStatus s =
                pos < state.trafficByPos.size()
                    ? state.trafficByPos[pos]
                    : TrafficStatus::Smooth;
            out << "    {\"pos\": " << pos << ", \"status\": "
                << static_cast<std::int32_t>(s) << '}';
            first = false;
        }
        if (!first) {
            out << '\n';
        }
        out << "  ]\n}\n";
        return out.str();
    }

    static bool writeMatchStateJson(
        const MatchState& state,
        const Field& field,
        const std::string& path) {
        return writeFile(path, toMatchStateJson(state, field));
    }
};

// ランダム問題を生成し、簡易サーバー用JSONへ保存する簡易入口。
inline GeneratedProblem create(
    const CreateConfig& config,
    const std::string& outputPath = "problem.json") {
    GeneratedProblem problem = ProblemCreator::generate(config);
    if (!ProblemCreator::writeJson(problem, outputPath)) {
        throw std::runtime_error("failed to write problem JSON: " + outputPath);
    }
    return problem;
}

inline bool createMatchSettingJson(
    const MatchSetting& setting,
    const std::string& outputPath = "match-setting.json") {
    return ProblemCreator::writeMatchSettingJson(setting, outputPath);
}

inline bool createMatchStateJson(
    const MatchState& state,
    const Field& field,
    const std::string& outputPath = "match-state.json") {
    return ProblemCreator::writeMatchStateJson(state, field, outputPath);
}

// 試合設定から日開始状態を生成し、そのままAPI形式のJSONへ保存する入口。
inline MatchState createMatchStateJson(
    const MatchSetting& setting,
    const CreateMatchStateConfig& config,
    const std::string& outputPath = "match-state.json") {
    MatchState state = ProblemCreator::generateMatchState(setting, config);
    if (!ProblemCreator::writeMatchStateJson(
            state, setting.field, outputPath)) {
        throw std::runtime_error(
            "failed to write match state JSON: " + outputPath);
    }
    return state;
}

}  // namespace benzen
