// Lexer and parser tests. Has its own main, so it stays out of the normal build:
//   g++ -std=c++17 -Wall -I. -o query_test tests/query_test.cpp QueryLexer.cpp QueryParser.cpp && ./query_test
#include "QueryParser.h"
#include <iostream>
#include <string>

static int failures = 0;

static void check(bool passed, const std::string& name) {
    std::cout << (passed ? "ok    " : "FAIL  ") << name << "\n";
    if (!passed) ++failures;
}

static void accepts(const std::string& input, CommandKind kind) {
    const ParseResult result = parse_command(input);
    check(result.ok && result.command.kind == kind, "accepts: " + input + (result.ok ? "" : "  [" + result.error + "]"));
}

static void rejects(const std::string& input, const std::string& message) {
    const ParseResult result = parse_command(input);
    check(!result.ok && result.error.find(message) != std::string::npos,
          "rejects: " + input + (result.ok ? "  [was accepted]" : "  [" + result.error + "]"));
}

int main() {
    accepts("FIND BOX x[-10,10] y[5,25] z[-2,3] WHERE intensity > 100 AND intensity < 200 LIMIT 5;", CommandKind::FIND);
    accepts("find box X[1,2];", CommandKind::FIND);
    accepts("FIND ALL LIMIT 100;", CommandKind::FIND);
    accepts("COUNT ALL;", CommandKind::COUNT);
    accepts("COUNT BOX x[-1.5e1, .5] WHERE metadata != 3;", CommandKind::COUNT);
    accepts("EXPLAIN FIND BOX x[0,1];", CommandKind::EXPLAIN);
    accepts("EXPLAIN COUNT ALL;", CommandKind::EXPLAIN);
    accepts("INSERT POINT (1.5, -2.5, 3) METADATA 42;", CommandKind::INSERT);
    accepts("INSERT POINT (1, 2, 3);", CommandKind::INSERT);
    accepts("LOAD '000000.bin';", CommandKind::LOAD);
    accepts("LOAD \"a;b.bin\";", CommandKind::LOAD);
    accepts("SHOW SHARDS;", CommandKind::SHOW_SHARDS);
    accepts("REBALANCE;", CommandKind::REBALANCE);
    accepts("rebalance;", CommandKind::REBALANCE);
    accepts("HELP;", CommandKind::HELP);
    accepts("exit;", CommandKind::EXIT);
    accepts("FIND BOX\n  x[0,1]\n  LIMIT 2\n;", CommandKind::FIND);

    rejects("FIND ALL LIMIT 5", "expected ';'");
    rejects("HELP", "expected ';'");
    rejects("HELP; HELP;", "expected end of command");
    rejects("FIND ALL;", "FIND ALL needs a LIMIT");
    rejects("FIND;", "expected BOX");
    rejects("FIND BOX;", "expected x, y or z");
    rejects("FIND BOX x[1,2] x[3,4];", "appears twice");
    rejects("FIND BOX x[5,1];", "min greater than max");
    rejects("FIND BOX x[1,2;", "expected ']'");
    rejects("FIND BOX w[1,2];", "expected x, y or z");
    rejects("FIND BOX x[1e50,2e50];", "too large");
    rejects("FIND ALL LIMIT 0;", "at least 1");
    rejects("FIND ALL LIMIT -1;", "whole number");
    rejects("FIND ALL LIMIT 2.5;", "whole number");
    rejects("COUNT ALL LIMIT 5;", "expected ';'");
    rejects("COUNT ALL WHERE x > 1;", "metadata or intensity");
    rejects("COUNT ALL WHERE metadata == 1;", "whole number");
    rejects("COUNT ALL WHERE metadata > 99999999999;", "too large");
    rejects("COUNT ALL WHERE metadata>1 AND metadata>2 AND metadata>3 AND metadata>4 AND metadata>5;", "at most 4");
    rejects("LOAD frame;", "quoted file path");
    rejects("LOAD 000000.bin;", "after number");
    rejects("LOAD 'unterminated;", "no closing quote");
    rejects("FIND @;", "unexpected character");
    rejects("FIND BOX x[1a,2];", "after number");
    rejects("DROP TABLE points;", "expected a command");
    rejects("SHOW;", "expected SHARDS");
    rejects("REBALANCE", "expected ';'");
    rejects("REBALANCE now;", "expected ';'");

    const ParseResult alias = parse_command("COUNT ALL WHERE intensity >= 7;");
    const ParseResult plain = parse_command("COUNT ALL WHERE metadata >= 7;");
    check(alias.ok && plain.ok && alias.command.filters[0].op == plain.command.filters[0].op
          && alias.command.filters[0].value == plain.command.filters[0].value, "intensity parses the same as metadata");

    const ParseResult partial = parse_command("COUNT BOX y[1,2];");
    check(partial.ok && partial.command.box.y_min == 1.0f && partial.command.box.x_min < -1.0e30f
          && partial.command.box.x_max > 1.0e30f, "an omitted axis covers its full range");

    const ParseResult located = parse_command("FIND BOX\n  x[1,2");
    check(!located.ok && located.error.find("line 2") != std::string::npos, "errors report the right line");

    std::cout << (failures == 0 ? "all passed\n" : std::to_string(failures) + " failed\n");
    return failures == 0 ? 0 : 1;
}
