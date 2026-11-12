#pragma once
#include "NetworkProtocol.h"
#include <cstdint>
#include <string>
#include <vector>

enum class CommandKind {
    FIND,
    COUNT,
    EXPLAIN,
    INSERT,
    LOAD,
    SHOW_SHARDS,
    REBALANCE,
    HELP,
    EXIT
};

struct Command {
    CommandKind kind;

    // FIND, COUNT and EXPLAIN
    bool all;                        // ALL rather than BOX
    QueryRequest box;                // an axis left out of BOX covers its full range
    std::vector<QueryFilter> filters;
    uint32_t limit;                  // 0 means no LIMIT

    // INSERT
    float x, y, z;
    uint32_t metadata;

    // LOAD
    std::string path;
    uint32_t dataset_id;             // 1 to MAX_DATASET_ID; 0 is kept for points that INSERT adds
};

struct ParseResult {
    bool ok;
    Command command;
    std::string error;
};

// Parses exactly one command, which must end with ';'
ParseResult parse_command(const std::string& input);
