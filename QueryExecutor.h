#pragma once
#include "Coordinator.h"
#include "MortonClient.h"
#include "QueryParser.h"
#include <istream>
#include <ostream>
#include <shared_mutex>
#include <string>

class QueryExecutor {
private:
    MortonClient& client;
    Coordinator& coordinator;
    std::ostream& out;
    std::string shard_map_path;
    std::shared_mutex& cluster_lock; // taken exclusively by REBALANCE; the HTTP thread reads under it

    SpatialQueryBuilder build_query(const Command& command);
    void run_find(const Command& command);
    void run_count(const Command& command);
    void run_explain(const Command& command);
    void run_insert(const Command& command);
    void run_load(const Command& command);
    void run_show_shards();
    void run_rebalance();
    void run_help();

public:
    QueryExecutor(MortonClient& client_ref, Coordinator& coordinator_ref, std::ostream& output, const std::string& map_path,
                  std::shared_mutex& lock);

    // False means the session should end
    bool run(const Command& command);
};

// Reads commands until EXIT or end of input. A command ends at ';', so it can span several lines.
// interactive means a person is at a terminal: it adds the prompt and line editing, and ignores in.
void run_shell(QueryExecutor& executor, std::istream& in, std::ostream& out, bool interactive);
