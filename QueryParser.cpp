#include "QueryParser.h"
#include "QueryLexer.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace {

struct ParseError {
    std::string message;
};

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

class Parser {
private:
    const std::vector<Token>& tokens;
    size_t pos = 0;

    const Token& current() const { return tokens[pos]; }
    void advance() { if (current().type != TokenType::END) ++pos; }

    [[noreturn]] void fail(const Token& at, const std::string& message) const {
        throw ParseError{"line " + std::to_string(at.line) + ", column " + std::to_string(at.column) + ": " + message};
    }

    std::string describe(const Token& token) const {
        return token.type == TokenType::END ? "end of input" : "'" + token.text + "'";
    }

    [[noreturn]] void expected(const std::string& what) const {
        fail(current(), "expected " + what + " but found " + describe(current()));
    }

    bool at_word(const char* word) const {
        return current().type == TokenType::WORD && upper(current().text) == word;
    }

    bool at_symbol(const char* symbol) const {
        return current().type == TokenType::SYMBOL && current().text == symbol;
    }

    void expect_word(const char* word) {
        if (!at_word(word)) expected(word);
        advance();
    }

    void expect_symbol(const char* symbol) {
        if (!at_symbol(symbol)) expected("'" + std::string(symbol) + "'");
        advance();
    }

    float parse_float() {
        if (current().type != TokenType::NUMBER) expected("a number");
        const float value = std::strtof(current().text.c_str(), nullptr);
        if (!std::isfinite(value)) fail(current(), "number is too large");
        advance();
        return value;
    }

    uint32_t parse_uint(const char* what) {
        const Token& token = current();
        const bool plain = token.type == TokenType::NUMBER
            && token.text.find_first_not_of("+0123456789") == std::string::npos;
        if (!plain) expected(std::string("a whole number for ") + what);

        errno = 0;
        const unsigned long long value = std::strtoull(token.text.c_str(), nullptr, 10);
        if (errno != 0 || value > std::numeric_limits<uint32_t>::max()) fail(token, "number is too large");
        advance();
        return static_cast<uint32_t>(value);
    }

    void parse_range(Command& command, bool (&seen)[3]) {
        const Token axis_token = current();
        const std::string axis = upper(axis_token.text);
        const int axis_index = axis == "X" ? 0 : axis == "Y" ? 1 : axis == "Z" ? 2 : -1;
        if (axis_token.type != TokenType::WORD || axis_index < 0) expected("x, y or z");
        if (seen[axis_index]) fail(axis_token, "axis " + axis_token.text + " appears twice");
        seen[axis_index] = true;
        advance();

        expect_symbol("[");
        const float low = parse_float();
        expect_symbol(",");
        const float high = parse_float();
        if (low > high) fail(axis_token, "axis " + axis_token.text + " has min greater than max");
        expect_symbol("]");

        float& box_min = axis_index == 0 ? command.box.x_min : axis_index == 1 ? command.box.y_min : command.box.z_min;
        float& box_max = axis_index == 0 ? command.box.x_max : axis_index == 1 ? command.box.y_max : command.box.z_max;
        box_min = low;
        box_max = high;
    }

    void parse_target(Command& command) {
        constexpr float max = std::numeric_limits<float>::max();
        command.box = {-max, max, -max, max, -max, max};

        if (at_word("ALL")) {
            command.all = true;
            advance();
            return;
        }

        expect_word("BOX");
        bool seen[3] = {false, false, false};
        parse_range(command, seen);
        while (at_word("X") || at_word("Y") || at_word("Z")) parse_range(command, seen);
    }

    FilterOp parse_op() {
        static const std::pair<const char*, FilterOp> ops[] = {
            {"=", FILTER_EQ}, {"!=", FILTER_NE}, {"<", FILTER_LT},
            {"<=", FILTER_LE}, {">", FILTER_GT}, {">=", FILTER_GE}
        };
        for (const auto& op : ops) {
            if (at_symbol(op.first)) {
                advance();
                return op.second;
            }
        }
        expected("a comparison (= != < <= > >=)");
    }

    void parse_condition(Command& command) {
        // intensity is the same field: KittiLoader stores reflectance in metadata
        if (!at_word("METADATA") && !at_word("INTENSITY")) expected("metadata or intensity");
        const Token field = current();
        advance();

        const FilterOp op = parse_op();
        const uint32_t value = parse_uint("the comparison");
        if (command.filters.size() >= MAX_QUERY_FILTERS) {
            fail(field, "a query can have at most " + std::to_string(MAX_QUERY_FILTERS) + " conditions");
        }
        command.filters.push_back({static_cast<uint8_t>(op), value});
    }

    void parse_where(Command& command) {
        if (!at_word("WHERE")) return;
        advance();
        parse_condition(command);
        while (at_word("AND")) {
            advance();
            parse_condition(command);
        }
    }

    void parse_find(Command& command) {
        const Token find = current();
        expect_word("FIND");
        parse_target(command);
        parse_where(command);
        if (at_word("LIMIT")) {
            advance();
            const Token value = current();
            command.limit = parse_uint("LIMIT");
            if (command.limit == 0) fail(value, "LIMIT must be at least 1");
        }
        // A typo must not be able to pull back the whole database in one response
        if (command.all && command.limit == 0) fail(find, "FIND ALL needs a LIMIT");
    }

    void parse_count(Command& command) {
        expect_word("COUNT");
        parse_target(command);
        parse_where(command);
    }

    void parse_insert(Command& command) {
        expect_word("INSERT");
        expect_word("POINT");
        expect_symbol("(");
        command.x = parse_float();
        expect_symbol(",");
        command.y = parse_float();
        expect_symbol(",");
        command.z = parse_float();
        expect_symbol(")");
        if (at_word("METADATA") || at_word("INTENSITY")) {
            advance();
            command.metadata = parse_uint("metadata");
        }
    }

    void parse_body(Command& command) {
        if (at_word("FIND")) {
            command.kind = CommandKind::FIND;
            parse_find(command);
        } else if (at_word("COUNT")) {
            command.kind = CommandKind::COUNT;
            parse_count(command);
        } else if (at_word("EXPLAIN")) {
            advance();
            if (at_word("FIND")) parse_find(command);
            else if (at_word("COUNT")) parse_count(command);
            else expected("FIND or COUNT");
            command.kind = CommandKind::EXPLAIN;
        } else if (at_word("INSERT")) {
            command.kind = CommandKind::INSERT;
            parse_insert(command);
        } else if (at_word("LOAD")) {
            command.kind = CommandKind::LOAD;
            advance();
            if (current().type != TokenType::STRING) expected("a quoted file path");
            command.path = current().text;
            advance();
        } else if (at_word("SHOW")) {
            command.kind = CommandKind::SHOW_SHARDS;
            advance();
            expect_word("SHARDS");
        } else if (at_word("REBALANCE")) {
            command.kind = CommandKind::REBALANCE;
            advance();
        } else if (at_word("HELP")) {
            command.kind = CommandKind::HELP;
            advance();
        } else if (at_word("EXIT")) {
            command.kind = CommandKind::EXIT;
            advance();
        } else {
            expected("a command (type HELP; for a list)");
        }
    }

public:
    explicit Parser(const std::vector<Token>& input) : tokens(input) {}

    Command run() {
        Command command{};
        command.metadata = 1;
        parse_body(command);
        expect_symbol(";");
        if (current().type != TokenType::END) expected("end of command");
        return command;
    }
};

} // namespace

ParseResult parse_command(const std::string& input) {
    ParseResult result{};

    const LexResult lexed = tokenize(input);
    if (!lexed.ok) {
        result.error = lexed.error;
        return result;
    }

    try {
        result.command = Parser(lexed.tokens).run();
        result.ok = true;
    } catch (const ParseError& error) {
        result.error = error.message;
    }
    return result;
}
