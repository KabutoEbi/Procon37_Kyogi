#pragma once

#include "game.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace procon37 {

class JsonInputError : public std::runtime_error {
public:
    explicit JsonInputError(const std::string& message)
        : std::runtime_error(message) {}
};

namespace json_detail {

struct Value {
    enum class Type {
        Null,
        Boolean,
        Integer,
        String,
        Array,
        Object,
    };

    Type type = Type::Null;
    bool boolean = false;
    std::int64_t integer = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;
};

class Parser final {
private:
    std::string_view text_;
    std::size_t pos_ = 0;
    std::size_t depth_ = 0;

    [[noreturn]] void fail(std::string_view message) const {
        throw JsonInputError(
            "JSON parse error at byte " + std::to_string(pos_) +
            ": " + std::string(message));
    }

    void skipSpace() noexcept {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
                break;
            }
            ++pos_;
        }
    }

    bool consume(char expected) noexcept {
        skipSpace();
        if (pos_ >= text_.size() || text_[pos_] != expected) {
            return false;
        }
        ++pos_;
        return true;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            fail(std::string("expected '") + expected + "'");
        }
    }

    static int hexValue(char c) noexcept {
        if ('0' <= c && c <= '9') {
            return c - '0';
        }
        if ('a' <= c && c <= 'f') {
            return c - 'a' + 10;
        }
        if ('A' <= c && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    std::uint32_t parseHex4() {
        if (text_.size() - pos_ < 4) {
            fail("incomplete unicode escape");
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = hexValue(text_[pos_++]);
            if (digit < 0) {
                fail("invalid unicode escape");
            }
            value = (value << 4U) | static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7fU) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7ffU) {
            out += static_cast<char>(0xc0U | (cp >> 6U));
            out += static_cast<char>(0x80U | (cp & 0x3fU));
        } else if (cp <= 0xffffU) {
            out += static_cast<char>(0xe0U | (cp >> 12U));
            out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU));
            out += static_cast<char>(0x80U | (cp & 0x3fU));
        } else {
            out += static_cast<char>(0xf0U | (cp >> 18U));
            out += static_cast<char>(0x80U | ((cp >> 12U) & 0x3fU));
            out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU));
            out += static_cast<char>(0x80U | (cp & 0x3fU));
        }
    }

    std::string parseString() {
        skipSpace();
        if (pos_ >= text_.size() || text_[pos_++] != '"') {
            fail("expected string");
        }

        std::string out;
        while (pos_ < text_.size()) {
            const unsigned char c =
                static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') {
                return out;
            }
            if (c < 0x20U) {
                fail("unescaped control character in string");
            }
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                fail("incomplete escape sequence");
            }
            const char escaped = text_[pos_++];
            switch (escaped) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t cp = parseHex4();
                    if (0xd800U <= cp && cp <= 0xdbffU) {
                        if (text_.size() - pos_ < 6 ||
                            text_[pos_] != '\\' ||
                            text_[pos_ + 1] != 'u') {
                            fail("missing low unicode surrogate");
                        }
                        pos_ += 2;
                        const std::uint32_t low = parseHex4();
                        if (low < 0xdc00U || low > 0xdfffU) {
                            fail("invalid low unicode surrogate");
                        }
                        cp = 0x10000U + ((cp - 0xd800U) << 10U) +
                             (low - 0xdc00U);
                    } else if (0xdc00U <= cp && cp <= 0xdfffU) {
                        fail("unexpected low unicode surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    fail("invalid escape sequence");
            }
        }
        fail("unterminated string");
    }

    Value parseInteger() {
        skipSpace();
        const std::size_t begin = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
            fail("invalid number");
        }
        if (text_[pos_] == '0') {
            ++pos_;
            if (pos_ < text_.size() &&
                '0' <= text_[pos_] && text_[pos_] <= '9') {
                fail("leading zero in number");
            }
        } else {
            while (pos_ < text_.size() &&
                   '0' <= text_[pos_] && text_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (pos_ < text_.size() &&
            (text_[pos_] == '.' || text_[pos_] == 'e' ||
             text_[pos_] == 'E')) {
            fail("non-integer number is not supported");
        }

        Value out;
        out.type = Value::Type::Integer;
        const char* first = text_.data() + begin;
        const char* last = text_.data() + pos_;
        const auto [end, err] =
            std::from_chars(first, last, out.integer);
        if (err != std::errc{} || end != last) {
            fail("integer is outside int64 range");
        }
        return out;
    }

    Value parseArray() {
        Value out;
        out.type = Value::Type::Array;
        expect('[');
        if (consume(']')) {
            return out;
        }
        while (true) {
            out.array.push_back(parseValue());
            if (consume(']')) {
                return out;
            }
            expect(',');
        }
    }

    Value parseObject() {
        Value out;
        out.type = Value::Type::Object;
        expect('{');
        if (consume('}')) {
            return out;
        }
        while (true) {
            const std::string key = parseString();
            for (const auto& entry : out.object) {
                if (entry.first == key) {
                    fail("duplicate object key: " + key);
                }
            }
            expect(':');
            out.object.emplace_back(key, parseValue());
            if (consume('}')) {
                return out;
            }
            expect(',');
        }
    }

    Value parseValue() {
        skipSpace();
        if (pos_ >= text_.size()) {
            fail("unexpected end of input");
        }
        if (++depth_ > 64) {
            fail("JSON nesting is too deep");
        }

        Value out;
        const char c = text_[pos_];
        if (c == '{') {
            out = parseObject();
        } else if (c == '[') {
            out = parseArray();
        } else if (c == '"') {
            out.type = Value::Type::String;
            out.string = parseString();
        } else if (c == '-' || ('0' <= c && c <= '9')) {
            out = parseInteger();
        } else if (text_.substr(pos_, 4) == "true") {
            pos_ += 4;
            out.type = Value::Type::Boolean;
            out.boolean = true;
        } else if (text_.substr(pos_, 5) == "false") {
            pos_ += 5;
            out.type = Value::Type::Boolean;
        } else if (text_.substr(pos_, 4) == "null") {
            pos_ += 4;
        } else {
            fail("invalid value");
        }
        --depth_;
        return out;
    }

public:
    explicit Parser(std::string_view text) : text_(text) {}

    Value parse() {
        Value out = parseValue();
        skipSpace();
        if (pos_ != text_.size()) {
            fail("trailing characters");
        }
        return out;
    }
};

[[noreturn]] inline void fail(
    std::string_view path,
    std::string_view message) {
    throw JsonInputError(
        std::string(path) + ": " + std::string(message));
}

inline const Value& object(const Value& v, std::string_view path) {
    if (v.type != Value::Type::Object) {
        fail(path, "expected object");
    }
    return v;
}

inline const Value& array(const Value& v, std::string_view path) {
    if (v.type != Value::Type::Array) {
        fail(path, "expected array");
    }
    return v;
}

inline const Value& member(
    const Value& obj,
    std::string_view key,
    std::string_view path) {
    object(obj, path);
    for (const auto& entry : obj.object) {
        if (entry.first == key) {
            return entry.second;
        }
    }
    fail(path, "missing required member '" + std::string(key) + "'");
}

inline std::int64_t integer(
    const Value& v,
    std::string_view path,
    std::int64_t min = std::numeric_limits<std::int64_t>::min(),
    std::int64_t max = std::numeric_limits<std::int64_t>::max()) {
    if (v.type != Value::Type::Integer) {
        fail(path, "expected integer");
    }
    if (v.integer < min || v.integer > max) {
        fail(path, "integer is outside allowed range");
    }
    return v.integer;
}

inline std::int32_t int32(
    const Value& v,
    std::string_view path,
    std::int32_t min = std::numeric_limits<std::int32_t>::min(),
    std::int32_t max = std::numeric_limits<std::int32_t>::max()) {
    return static_cast<std::int32_t>(integer(v, path, min, max));
}

inline std::string childPath(
    std::string_view path,
    std::string_view key) {
    return std::string(path) + '.' + std::string(key);
}

inline std::string indexPath(std::string_view path, std::size_t i) {
    return std::string(path) + '[' + std::to_string(i) + ']';
}

inline std::vector<std::int32_t> intArray(
    const Value& v,
    std::string_view path,
    std::int32_t min,
    std::int32_t max) {
    array(v, path);
    std::vector<std::int32_t> out;
    out.reserve(v.array.size());
    for (std::size_t i = 0; i < v.array.size(); ++i) {
        out.push_back(int32(v.array[i], indexPath(path, i), min, max));
    }
    return out;
}

inline std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw JsonInputError("failed to open JSON file: " + path);
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        throw JsonInputError("failed to get JSON file size: " + path);
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    in.seekg(0, std::ios::beg);
    if (!text.empty() && !in.read(text.data(), size)) {
        throw JsonInputError("failed to read JSON file: " + path);
    }
    return text;
}

inline Agent parseAgent(
    const Value& v,
    std::string_view path,
    AgentIndex index,
    const MatchSetting& s) {
    object(v, path);
    const std::int32_t kind = int32(
        member(v, "kind", path), childPath(path, "kind"), 0, 1);
    const CellPos pos = int32(
        member(v, "pos", path), childPath(path, "pos"));
    const std::int32_t fuel = int32(
        member(v, "fuel", path),
        childPath(path, "fuel"),
        0,
        s.fuelLimit);
    if (!s.field.contains(pos) || !s.field.at(pos).isPassable()) {
        fail(childPath(path, "pos"), "position is not passable");
    }
    return Agent{index, static_cast<AgentKind>(kind), pos, fuel};
}

inline std::vector<Agent> parseAgents(
    const Value& v,
    std::string_view path,
    const MatchSetting& s) {
    array(v, path);
    if (v.array.size() != s.agentCount()) {
        fail(path, "agent count does not match setting");
    }
    std::vector<Agent> out;
    out.reserve(v.array.size());
    for (std::size_t i = 0; i < v.array.size(); ++i) {
        out.push_back(parseAgent(v.array[i], indexPath(path, i), i, s));
    }
    return out;
}

}  // namespace json_detail

class JsonInput final {
public:
    static MatchSetting parseMatchSetting(std::string_view text) {
        using namespace json_detail;
        const Value root = Parser(text).parse();
        object(root, "$");

        MatchSetting s;
        s.startsAt = integer(
            member(root, "startsAt", "$"), "$.startsAt", 0);
        s.daySeconds = intArray(
            member(root, "daySeconds", "$"),
            "$.daySeconds",
            1,
            std::numeric_limits<std::int32_t>::max());
        s.daySteps = intArray(
            member(root, "daySteps", "$"),
            "$.daySteps",
            1,
            std::numeric_limits<std::int32_t>::max());
        if (s.daySteps.size() != s.daySeconds.size() ||
            s.daySteps.size() < 4 || s.daySteps.size() > 10) {
            fail("$.daySteps", "day arrays must have the same length in [4, 10]");
        }

        const Value& map = member(root, "map", "$");
        object(map, "$.map");
        s.field.height = int32(
            member(map, "height", "$.map"), "$.map.height", 8, 32);
        s.field.width = int32(
            member(map, "width", "$.map"), "$.map.width", 8, 32);
        const Value& cells = member(map, "cells", "$.map");
        array(cells, "$.map.cells");
        if (cells.array.size() != static_cast<std::size_t>(s.field.height)) {
            fail("$.map.cells", "row count does not match height");
        }
        s.field.cells.reserve(s.field.cellCount());
        for (std::size_t y = 0; y < cells.array.size(); ++y) {
            const std::string rowPath = indexPath("$.map.cells", y);
            const Value& row = array(cells.array[y], rowPath);
            if (row.array.size() != static_cast<std::size_t>(s.field.width)) {
                fail(rowPath, "column count does not match width");
            }
            for (std::size_t x = 0; x < row.array.size(); ++x) {
                const auto type = int32(
                    row.array[x], indexPath(rowPath, x), 0, 3);
                s.field.cells.push_back(
                    Cell{static_cast<CellType>(type), -1});
            }
        }

        const Value& spots = member(root, "spots", "$");
        array(spots, "$.spots");
        if (spots.array.size() > kMaxSpots) {
            fail("$.spots", "too many spots");
        }
        s.field.spots.reserve(spots.array.size());
        for (std::size_t i = 0; i < spots.array.size(); ++i) {
            const std::string path = indexPath("$.spots", i);
            const Value& spot = object(spots.array[i], path);
            s.field.spots.push_back(Spot{
                int32(member(spot, "brand", path), childPath(path, "brand")),
                int32(member(spot, "pos", path), childPath(path, "pos")),
                int32(
                    member(spot, "stocks", path),
                    childPath(path, "stocks"),
                    1,
                    8),
                -1,
            });
        }
        if (!s.field.rebuildSpotIndex()) {
            fail("$.spots", "spot positions, stocks, or brands are invalid");
        }

        s.agentStarts = intArray(
            member(root, "agents", "$"),
            "$.agents",
            0,
            static_cast<std::int32_t>(s.field.cellCount() - 1));
        if (s.agentCount() < 3 || s.agentCount() > kMaxAgents) {
            fail("$.agents", "agent count must be in [3, 8]");
        }
        std::unordered_set<CellPos> starts;
        for (std::size_t i = 0; i < s.agentStarts.size(); ++i) {
            const CellPos pos = s.agentStarts[i];
            if (s.field.at(pos).type != CellType::Plain ||
                s.field.at(pos).hasSpot() || !starts.insert(pos).second) {
                fail(indexPath("$.agents", i),
                     "start must be a unique plain cell without a spot");
            }
        }

        s.fuelLimit = int32(
            member(root, "fuelLimits", "$"),
            "$.fuelLimits",
            1,
            std::numeric_limits<std::int32_t>::max());
        s.playerCount = int32(
            member(root, "players", "$"), "$.players", 1);
        s.busyThreshold = int32(
            member(root, "busyThreshold", "$"),
            "$.busyThreshold",
            1,
            5);
        s.jammedThreshold = int32(
            member(root, "jammedThreshold", "$"),
            "$.jammedThreshold",
            2,
            10);
        if (s.jammedThreshold <= s.busyThreshold) {
            fail("$.jammedThreshold", "must be greater than busyThreshold");
        }

        const std::int32_t base = s.field.width + s.field.height;
        for (std::size_t i = 0; i < s.daySteps.size(); ++i) {
            if (s.daySteps[i] < base || s.daySteps[i] > base * 4) {
                fail(indexPath("$.daySteps", i),
                     "step count is outside map-dependent range");
            }
        }
        if (s.fuelLimit < s.daySteps.front() ||
            s.fuelLimit > s.daySteps.front() * 3) {
            fail("$.fuelLimits", "fuel limit is outside first-day range");
        }
        if (s.field.spots.size() < s.agentCount() ||
            s.field.spots.size() > static_cast<std::size_t>(
                                       std::max(s.field.width, s.field.height))) {
            fail("$.spots", "spot count is outside allowed range");
        }
        for (std::size_t i = 0; i < s.field.spots.size(); ++i) {
            if (s.field.spots[i].maxStock >
                static_cast<std::int32_t>(s.agentCount())) {
                fail(childPath(indexPath("$.spots", i), "stocks"),
                     "stock exceeds agent count");
            }
        }
        return s;
    }

    static MatchState parseMatchState(
        std::string_view text,
        const MatchSetting& s) {
        using namespace json_detail;
        const Value root = Parser(text).parse();
        object(root, "$");

        MatchState st;
        st.endsAt = integer(
            member(root, "endsAt", "$"), "$.endsAt", 0);
        st.day = int32(
            member(root, "day", "$"),
            "$.day",
            0,
            static_cast<std::int32_t>(s.dayCount() - 1));
        st.agents = parseAgents(
            member(root, "agents", "$"), "$.agents", s);

        const Value& others = member(root, "others", "$");
        array(others, "$.others");
        if (others.array.size() !=
            static_cast<std::size_t>(s.playerCount - 1)) {
            fail("$.others", "other player count does not match setting");
        }
        std::unordered_set<PlayerId> ids;
        st.others.reserve(others.array.size());
        for (std::size_t i = 0; i < others.array.size(); ++i) {
            const std::string path = indexPath("$.others", i);
            const Value& team = object(others.array[i], path);
            const PlayerId id = int32(
                member(team, "id", path), childPath(path, "id"), 0);
            if (!ids.insert(id).second) {
                fail(childPath(path, "id"), "duplicate player id");
            }
            st.others.push_back(TeamState{
                id,
                parseAgents(
                    member(team, "agents", path),
                    childPath(path, "agents"),
                    s),
            });
        }

        const Value& traffics = member(root, "traffics", "$");
        array(traffics, "$.traffics");
        st.trafficByPos.assign(
            s.field.cellCount(), TrafficStatus::Smooth);
        std::vector<bool> seen(s.field.cellCount(), false);
        for (std::size_t i = 0; i < traffics.array.size(); ++i) {
            const std::string path = indexPath("$.traffics", i);
            const Value& traffic = object(traffics.array[i], path);
            const CellPos pos = int32(
                member(traffic, "pos", path),
                childPath(path, "pos"),
                0,
                static_cast<std::int32_t>(s.field.cellCount() - 1));
            if (s.field.at(pos).type != CellType::Road) {
                fail(childPath(path, "pos"), "traffic cell is not a road");
            }
            const auto index = static_cast<std::size_t>(pos);
            if (seen[index]) {
                fail(childPath(path, "pos"), "duplicate traffic cell");
            }
            seen[index] = true;
            st.trafficByPos[index] = static_cast<TrafficStatus>(int32(
                member(traffic, "status", path),
                childPath(path, "status"),
                0,
                2));
        }
        for (std::size_t pos = 0; pos < s.field.cellCount(); ++pos) {
            if (s.field.cells[pos].type == CellType::Road && !seen[pos]) {
                fail("$.traffics", "road cell is missing from traffic list");
            }
        }
        return st;
    }

    static MatchSetting readMatchSetting(const std::string& path) {
        return parseMatchSetting(json_detail::readFile(path));
    }

    static MatchState readMatchState(
        const std::string& path,
        const MatchSetting& setting) {
        return parseMatchState(json_detail::readFile(path), setting);
    }
};

inline MatchSetting parseMatchSettingJson(std::string_view text) {
    return JsonInput::parseMatchSetting(text);
}

inline MatchState parseMatchStateJson(
    std::string_view text,
    const MatchSetting& setting) {
    return JsonInput::parseMatchState(text, setting);
}

inline MatchSetting readMatchSettingJson(const std::string& path) {
    return JsonInput::readMatchSetting(path);
}

inline MatchState readMatchStateJson(
    const std::string& path,
    const MatchSetting& setting) {
    return JsonInput::readMatchState(path, setting);
}

}  // namespace procon37
