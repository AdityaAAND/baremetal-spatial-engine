#include "QueryExecutor.h"
#include "KittiLoader.h"
#include "LineEditor.h"
#include "MortonCode.h"
#include "QueryOptimizer.h"
#include <algorithm>
#include <cctype>
#include <string>

// Keeps a large unlimited result from flooding the terminal; a query with LIMIT prints every row
static constexpr size_t MAX_ROWS_SHOWN = 50;

QueryExecutor::QueryExecutor(MortonClient& client_ref, Coordinator& coordinator_ref, std::ostream& output, const std::string& map_path)
    : client(client_ref), coordinator(coordinator_ref), out(output), shard_map_path(map_path) {}

SpatialQueryBuilder QueryExecutor::build_query(const Command& command) {
    SpatialQueryBuilder query = client.query();
    if (command.all) {
        query.all();
    } else {
        const QueryRequest& box = command.box;
        query.bounding_box(box.x_min, box.x_max, box.y_min, box.y_max, box.z_min, box.z_max);
    }
    for (const QueryFilter& filter : command.filters) {
        query.where(static_cast<FilterOp>(filter.op), filter.value);
    }
    query.limit(command.limit);
    return query;
}

void QueryExecutor::run_find(const Command& command) {
    const QueryResult result = build_query(command).execute();
    if (!result.ok) {
        out << "error: " << result.error << "\n";
        return;
    }

    const size_t total = result.records.size();
    const size_t shown = command.limit == 0 ? std::min(total, MAX_ROWS_SHOWN) : total;
    for (size_t i = 0; i < shown; ++i) {
        const SpatialRecord& rec = result.records[i];
        out << "(" << rec.x << ", " << rec.y << ", " << rec.z << ")  metadata=" << rec.metadata << "\n";
    }
    if (shown < total) {
        out << "showing the first " << shown << " of " << total << " rows; add LIMIT to choose how many\n";
    } else {
        out << total << (total == 1 ? " row\n" : " rows\n");
    }
}

void QueryExecutor::run_count(const Command& command) {
    const QueryResult result = build_query(command).execute_count();
    if (!result.ok) {
        out << "error: " << result.error << "\n";
        return;
    }
    out << result.count << "\n";
}

void QueryExecutor::run_explain(const Command& command) {
    const ExecutionPlan plan = QueryOptimizer::optimize(command.box);
    out << plan.description << "\n";
    if (plan.plan_type != ExecutionPlanType::INVALID_PLAN) {
        out << "key span: " << plan.key_span << ", scan cost: " << plan.scan_cost << "\n";
    }
}

void QueryExecutor::run_insert(const Command& command) {
    // insert() only reports false, so catch the one failure that has a useful explanation here
    if (!coord_in_range(command.x) || !coord_in_range(command.y) || !coord_in_range(command.z)) {
        out << "error: coordinates must be between about -1000 and 1097\n";
        return;
    }
    if (client.insert(command.x, command.y, command.z, 0, command.metadata)) {
        out << "inserted 1 point\n";
    } else {
        out << "error: insert was not committed\n";
    }
}

void QueryExecutor::run_load(const Command& command) {
    const KittiLoadResult result = KittiLoader::load_bin_file(client, command.path);
    if (!result.success) {
        out << "error: could not load " << command.path << "\n";
        return;
    }
    out << "loaded " << result.points_inserted << " of " << result.total_points_read << " points in "
        << static_cast<uint64_t>(result.elapsed_ms) << " ms\n";
}

void QueryExecutor::run_show_shards() {
    for (uint32_t node : coordinator.node_ids()) {
        uint64_t points = 0;
        if (!coordinator.node_point_count(node, points)) {
            out << "error: node " << node << " did not answer\n";
            continue;
        }

        std::string ranges;
        for (const ShardRange& shard : coordinator.get_shards()) {
            if (shard.node_id != node) continue;
            if (!ranges.empty()) ranges += " and ";
            ranges += std::to_string(shard.start_key) + " to " + std::to_string(shard.end_key);
        }
        out << "node " << node << ": " << points << " points, " << (ranges.empty() ? "no keys" : "keys " + ranges) << "\n";
    }
}

void QueryExecutor::run_rebalance() {
    const RebalanceResult result = coordinator.rebalance(shard_map_path);
    if (!result.ok) {
        out << "error: " << result.error << "\n";
        return;
    }
    out << "rebalanced " << result.total_points << " points across " << coordinator.node_ids().size()
        << " workers (" << result.moved_points << " moved)\n";
}

void QueryExecutor::run_help() {
    out << "Every command ends with ';' and can span several lines.\n"
        << "  FIND BOX x[-10,10] y[5,25] z[-2,3] WHERE intensity > 100 LIMIT 5;\n"
        << "  FIND ALL LIMIT 100;\n"
        << "  COUNT BOX x[-10,10] y[5,25];\n"
        << "  COUNT ALL WHERE metadata >= 50;\n"
        << "  EXPLAIN FIND BOX x[-10,10] y[5,25] z[-2,3];\n"
        << "  INSERT POINT (1.5, 2.5, 3.5) METADATA 42;\n"
        << "  LOAD '000000.bin';\n"
        << "  SHOW SHARDS;\n"
        << "  REBALANCE;\n"
        << "  HELP;\n"
        << "  EXIT;\n"
        << "An axis left out of BOX covers its full range. WHERE compares metadata\n"
        << "(or its alias intensity) using = != < <= > >=, with up to 4 joined by AND.\n";
}

bool QueryExecutor::run(const Command& command) {
    switch (command.kind) {
        case CommandKind::FIND:        run_find(command); break;
        case CommandKind::COUNT:       run_count(command); break;
        case CommandKind::EXPLAIN:     run_explain(command); break;
        case CommandKind::INSERT:      run_insert(command); break;
        case CommandKind::LOAD:        run_load(command); break;
        case CommandKind::SHOW_SHARDS: run_show_shards(); break;
        case CommandKind::REBALANCE:   run_rebalance(); break;
        case CommandKind::HELP:        run_help(); break;
        case CommandKind::EXIT:        return false;
    }
    return true;
}

static bool is_blank(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

// False means the session should end. A parse error only skips this command.
static bool run_statement(QueryExecutor& executor, const std::string& statement, std::ostream& out) {
    const ParseResult parsed = parse_command(statement);
    if (!parsed.ok) {
        out << "error: " << parsed.error << "\n";
        return true;
    }
    return executor.run(parsed.command);
}

void run_shell(QueryExecutor& executor, std::istream& in, std::ostream& out, bool interactive) {
    LineEditor editor(out);
    std::string statement;
    std::string line;

    while (true) {
        if (interactive) {
            const ReadStatus status = editor.read_line(is_blank(statement) ? "spatial> " : "   ...> ", line);
            if (status == ReadStatus::END) break;
            if (status == ReadStatus::CANCELLED) {
                statement.clear();
                continue;
            }
        } else if (!std::getline(in, line)) {
            break;
        }

        char quote = '\0';
        for (const char c : line) {
            statement += c;
            if (quote != '\0') {
                if (c == quote) quote = '\0';
            } else if (c == '\'' || c == '"') {
                quote = c;
            } else if (c == ';') {
                const bool keep_going = is_blank(statement.substr(0, statement.size() - 1))
                    || run_statement(executor, statement, out);
                statement.clear();
                if (!keep_going) return;
            }
        }

        // Strings can't span lines, so waiting for more input would hang; let the lexer report it
        if (quote != '\0') {
            run_statement(executor, statement, out);
            statement.clear();
        } else if (is_blank(statement)) {
            statement.clear(); // otherwise the next command's errors report the wrong line
        } else {
            statement += '\n';
        }
    }

    if (!is_blank(statement)) out << "error: input ended before ';', so the last command was not run\n";
}
