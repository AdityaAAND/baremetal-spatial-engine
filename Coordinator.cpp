#include "Coordinator.h"
#include "MortonCode.h"
#include "NetworkClient.h"
#include <iostream>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <set>

// Longer than the worker's own wait on the target, so the worker reports a stuck target first
static constexpr int MIGRATE_TIMEOUT_SEC = 60;

// Generated ids start above the small hand-picked ids callers pass to execute_2pc_transaction
Coordinator::Coordinator() : last_tx_id(1ULL << 32) {}

void Coordinator::register_node(uint32_t node_id, const NodeEndpoint& endpoint) {
    node_endpoints[node_id] = endpoint;
}

void Coordinator::register_shard(uint32_t node_id, uint64_t start_key, uint64_t end_key) {
    cluster_shards.push_back({node_id, start_key, end_key});
}

uint32_t Coordinator::route_query(float x, float y, float z) {
    uint32_t x_int = quantize_coord(x);
    uint32_t y_int = quantize_coord(y);
    uint32_t z_int = quantize_coord(z);

    uint64_t target_key = calculate_z_order(x_int, y_int, z_int);

    for (const auto& shard : cluster_shards) {
        if (target_key >= shard.start_key && target_key <= shard.end_key) {
            return shard.node_id;
        }
    }
    return 0;
}

bool Coordinator::split_shard(uint32_t old_node_id, uint32_t new_node_id, NodeEndpoint old_node, NodeEndpoint new_node) {
    std::cout << "\n[Coordinator] Triggering Dynamic Re-Sharding for Node " << old_node_id << "...\n";

    auto it = std::find_if(cluster_shards.begin(), cluster_shards.end(),
                           [&](const ShardRange& s) { return s.node_id == old_node_id; });
    if (it == cluster_shards.end()) {
        std::cout << "[Coordinator] Split aborted: no shard owned by Node " << old_node_id << "\n";
        return false;
    }

    // Copy the bounds: the push_back below can reallocate the vector and invalidate `it`
    const uint64_t start_key = it->start_key;
    const uint64_t end_key = it->end_key;
    if (start_key >= end_key) {
        std::cout << "[Coordinator] Split aborted: shard [" << start_key << ".." << end_key << "] is too small to split\n";
        return false;
    }
    const uint64_t mid_key = start_key + (end_key - start_key) / 2;

    // 1. Ask the old node to move keys [mid_key + 1 .. end_key] to the new node
    NetworkClient client;
    if (!client.connect_to_node(old_node.ip.c_str(), old_node.port)) {
        std::cout << "[Coordinator] Split aborted: cannot reach Node " << old_node_id << " at " << old_node.ip << ":" << old_node.port << "\n";
        return false;
    }

    MigrateRangePayload payload{};
    payload.min_key = mid_key + 1;
    payload.max_key = end_key;
    snprintf(payload.target_ip, sizeof(payload.target_ip), "%s", new_node.ip.c_str());
    payload.target_port = new_node.port;

    bool migrated = client.send_migrate(payload);
    client.close_connection();
    if (!migrated) {
        std::cout << "[Coordinator] Split aborted: migration to Node " << new_node_id << " was not acknowledged; shard map unchanged\n";
        return false;
    }

    // 2. Only now that the data has moved, update the shard map
    it->end_key = mid_key;
    cluster_shards.push_back({new_node_id, mid_key + 1, end_key});

    std::cout << "[Coordinator] Shard Split Completed!\n"
              << "   Node " << old_node_id << " Range: [" << start_key << ".." << mid_key << "]\n"
              << "   Node " << new_node_id << " Range: [" << mid_key + 1 << ".." << end_key << "]\n\n";
    return true;
}

void Coordinator::failover_primary_to_standby(uint32_t failed_node_id, uint32_t standby_node_id) {
    std::cout << "\n[Coordinator ALERT] Primary Worker Node " << failed_node_id << " UNRESPONSIVE!\n";
    std::cout << "[Coordinator] Initiating Failover Protocol -> Remapping Spatial Shards to Standby Node " << standby_node_id << "...\n";

    for (auto& shard : cluster_shards) {
        if (shard.node_id == failed_node_id) {
            shard.node_id = standby_node_id;
            std::cout << "[Coordinator] Shard range [" << shard.start_key << ".." << shard.end_key 
                      << "] successfully remapped to Standby Node " << standby_node_id << "\n";
        }
    }
}

bool Coordinator::execute_2pc_transaction(uint64_t tx_id, const std::vector<SpatialRecord>& records) {
    // Group records by owning shard; refuse the whole transaction if any record cannot be placed
    std::map<uint32_t, std::vector<SpatialRecord>> records_by_node;
    for (const auto& rec : records) {
        uint32_t node_id = route_query(rec.x, rec.y, rec.z);
        if (node_id == 0 || node_endpoints.count(node_id) == 0) {
            std::cout << "[Coordinator] TxID=" << tx_id << " ABORTED before Phase 1: no shard/endpoint for point ("
                      << rec.x << ", " << rec.y << ", " << rec.z << ")\n";
            return false;
        }
        records_by_node[node_id].push_back(rec);
    }

    std::cout << "\n[Coordinator] === Starting 2PC Transaction TxID=" << tx_id << ": " << records.size()
              << " record(s) across " << records_by_node.size() << " shard(s) ===\n";

    std::vector<uint32_t> nodes;
    for (const auto& entry : records_by_node) nodes.push_back(entry.first);

    std::vector<NetworkClient> clients(nodes.size());
    bool all_voted_commit = true;

    // --- PHASE 1: PREPARE (only the shards that own records in this transaction) ---
    for (size_t i = 0; i < nodes.size() && all_voted_commit; ++i) {
        const NodeEndpoint& ep = node_endpoints.at(nodes[i]);
        if (!clients[i].connect_to_node(ep.ip.c_str(), ep.port)) {
            std::cout << "[Coordinator] Phase 1: cannot reach Node " << nodes[i] << "\n";
            all_voted_commit = false;
            break;
        }

        for (const auto& rec : records_by_node[nodes[i]]) {
            if (clients[i].send_prepare(tx_id, rec) != CMD_VOTE_COMMIT) {
                std::cout << "[Coordinator] Phase 1: Node " << nodes[i] << " voted VOTE_ABORT\n";
                all_voted_commit = false;
                break;
            }
        }
        if (all_voted_commit) {
            std::cout << "[Coordinator] Phase 1: Node " << nodes[i] << " voted VOTE_COMMIT for "
                      << records_by_node[nodes[i]].size() << " record(s)\n";
        }
    }

    // --- PHASE 2: COMMIT OR ABORT ---
    if (all_voted_commit) {
        // The COMMIT decision is final. A missing ACK leaves that node's outcome unknown, so retry once
        // on a fresh connection; the worker's CMD_COMMIT is idempotent (an unknown tx_id is just ACKed).
        bool all_acked = true;
        for (size_t i = 0; i < nodes.size(); ++i) {
            bool acked = clients[i].send_commit(tx_id);
            clients[i].close_connection();

            if (!acked) {
                const NodeEndpoint& ep = node_endpoints.at(nodes[i]);
                NetworkClient retry;
                acked = retry.connect_to_node(ep.ip.c_str(), ep.port) && retry.send_commit(tx_id);
            }
            if (!acked) {
                all_acked = false;
                std::cout << "[Coordinator] WARNING: Node " << nodes[i]
                          << " did not ACK COMMIT for TxID=" << tx_id << " (outcome on that node unknown)\n";
            }
        }
        if (!all_acked) {
            std::cout << "[Coordinator] TxID=" << tx_id << " decided COMMIT, but not all participants confirmed it\n";
        }
        std::cout << "[Coordinator] === TxID=" << tx_id << " COMMITTED ===\n";
        return true;
    } else {
        // Unconnected clients are skipped inside send_abort (sock_fd < 0)
        for (size_t i = 0; i < nodes.size(); ++i) {
            clients[i].send_abort(tx_id);
            clients[i].close_connection();
        }
        std::cout << "[Coordinator] === TxID=" << tx_id << " ABORTED ===\n";
        return false;
    }
}

std::vector<uint32_t> Coordinator::node_ids() const {
    std::vector<uint32_t> ids;
    for (const auto& entry : node_endpoints) ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool Coordinator::node_point_count(uint32_t node_id, uint64_t& count) {
    auto endpoint = node_endpoints.find(node_id);
    if (endpoint == node_endpoints.end()) return false;

    constexpr float max = std::numeric_limits<float>::max();
    FilteredQueryRequest req{};
    req.box = {-max, max, -max, max, -max, max};
    req.count_only = 1;

    NetworkClient client;
    std::vector<SpatialRecord> unused;
    return client.connect_to_node(endpoint->second.ip.c_str(), endpoint->second.port)
        && client.send_filtered_query(req, unused, count);
}

RebalanceResult Coordinator::rebalance(const std::string& map_path) {
    RebalanceResult result{};
    const std::vector<uint32_t> nodes = node_ids();

    // 1. Learn where the data is. Equal key ranges would not help: points cluster in a narrow band of keys.
    std::map<uint32_t, std::vector<uint64_t>> held;
    std::vector<uint64_t> all_keys;
    for (uint32_t node : nodes) {
        const NodeEndpoint& ep = node_endpoints.at(node);
        NetworkClient client;
        if (!client.connect_to_node(ep.ip.c_str(), ep.port) || !client.send_get_keys(held[node])) {
            result.error = "cannot read keys from node " + std::to_string(node);
            return result;
        }
        all_keys.insert(all_keys.end(), held[node].begin(), held[node].end());
    }

    // A key on two nodes is a leftover of a failed move, and migrating it merges the copies
    std::sort(all_keys.begin(), all_keys.end());
    all_keys.erase(std::unique(all_keys.begin(), all_keys.end()), all_keys.end());
    if (all_keys.size() < nodes.size()) {
        result.error = "need at least " + std::to_string(nodes.size()) + " points to rebalance "
                     + std::to_string(nodes.size()) + " workers";
        return result;
    }

    // 2. Cut the sorted keys into equal groups
    std::vector<ShardRange> new_shards;
    uint64_t next_start = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const bool last = i + 1 == nodes.size();
        const uint64_t end_key = last ? MAX_MORTON_KEY : all_keys[all_keys.size() * (i + 1) / nodes.size() - 1];
        new_shards.push_back({nodes[i], next_start, end_key});
        next_start = end_key + 1;
    }

    // 3. Each node sends away what it no longer owns. Only one connection is open at a time, because
    // the source connects to the target itself and a worker serves one connection at a time.
    for (uint32_t source : nodes) {
        const NodeEndpoint& ep = node_endpoints.at(source);
        const std::vector<uint64_t>& keys = held[source];

        for (const ShardRange& shard : new_shards) {
            if (shard.node_id == source) continue;
            const auto first = std::lower_bound(keys.begin(), keys.end(), shard.start_key);
            const auto past_last = std::upper_bound(keys.begin(), keys.end(), shard.end_key);
            if (first == past_last) continue;

            const NodeEndpoint& target = node_endpoints.at(shard.node_id);
            MigrateRangePayload payload{};
            payload.min_key = shard.start_key;
            payload.max_key = shard.end_key;
            snprintf(payload.target_ip, sizeof(payload.target_ip), "%s", target.ip.c_str());
            payload.target_port = static_cast<uint32_t>(target.port);

            NetworkClient client;
            bool moved = client.connect_to_node(ep.ip.c_str(), ep.port);
            if (moved) {
                client.set_receive_timeout(MIGRATE_TIMEOUT_SEC);
                moved = client.send_migrate(payload);
            }
            if (!moved) {
                result.error = "moving points from node " + std::to_string(source) + " to node "
                             + std::to_string(shard.node_id) + " failed; queries may miss points until REBALANCE succeeds";
                return result;
            }
            result.moved_points += static_cast<uint64_t>(past_last - first);
        }
    }

    // 4. Trust the workers' counts over the plan before publishing the new map
    uint64_t counted = 0;
    for (uint32_t node : nodes) {
        uint64_t count = 0;
        if (!node_point_count(node, count)) {
            result.error = "cannot confirm the point count on node " + std::to_string(node);
            return result;
        }
        counted += count;
    }
    if (counted != all_keys.size()) {
        result.error = "expected " + std::to_string(all_keys.size()) + " points after moving but found " + std::to_string(counted);
        return result;
    }

    cluster_shards = new_shards;
    if (!save_shard_map(map_path)) {
        result.error = "points moved, but the shard map could not be saved to " + map_path;
        return result;
    }

    result.ok = true;
    result.total_points = all_keys.size();
    return result;
}

bool Coordinator::save_shard_map(const std::string& path) const {
    // Rename is atomic, so a crash mid-write cannot leave a half-written map behind
    const std::string temp_path = path + ".tmp";
    {
        std::ofstream file(temp_path);
        for (const auto& shard : cluster_shards) {
            file << shard.node_id << " " << shard.start_key << " " << shard.end_key << "\n";
        }
        if (!file.good()) return false;
    }
    return std::rename(temp_path.c_str(), path.c_str()) == 0;
}

bool Coordinator::load_shard_map(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::vector<ShardRange> loaded;
    ShardRange shard{};
    while (file >> shard.node_id >> shard.start_key >> shard.end_key) loaded.push_back(shard);
    if (!file.eof() || loaded.empty()) return false;

    cluster_shards = loaded;
    return true;
}

uint64_t Coordinator::next_tx_id() {
    return ++last_tx_id;
}

std::set<uint32_t> Coordinator::overlapping_nodes(const QueryRequest& box) const {
    const uint64_t min_key = point_key(box.x_min, box.y_min, box.z_min);
    const uint64_t max_key = point_key(box.x_max, box.y_max, box.z_max);

    // A set because a node owns several ranges after a split, and must only be asked once
    std::set<uint32_t> nodes;
    for (const auto& shard : cluster_shards) {
        if (shard.start_key <= max_key && shard.end_key >= min_key) nodes.insert(shard.node_id);
    }
    return nodes;
}

bool Coordinator::execute_query(const QueryRequest& box, std::vector<SpatialRecord>& results) {
    for (uint32_t node_id : overlapping_nodes(box)) {
        auto endpoint = node_endpoints.find(node_id);
        NetworkClient client;
        std::vector<SpatialRecord> matches;
        if (endpoint == node_endpoints.end()
            || !client.connect_to_node(endpoint->second.ip.c_str(), endpoint->second.port)
            || !client.send_query(box, matches)) {
            std::cout << "[Coordinator] Query failed: no answer from Node " << node_id << "\n";
            return false;
        }
        results.insert(results.end(), matches.begin(), matches.end());
    }
    return true;
}

bool Coordinator::execute_filtered_query(const FilteredQueryRequest& req, std::vector<SpatialRecord>& results, uint64_t& count) {
    uint64_t found = 0;
    for (uint32_t node_id : overlapping_nodes(req.box)) {
        if (req.limit != 0 && found >= req.limit) break;

        // Asking each shard only for what is still missing means the rows never overshoot the limit
        FilteredQueryRequest shard_req = req;
        if (req.limit != 0) shard_req.limit = req.limit - static_cast<uint32_t>(found);

        auto endpoint = node_endpoints.find(node_id);
        NetworkClient client;
        std::vector<SpatialRecord> matches;
        uint64_t shard_count = 0;
        if (endpoint == node_endpoints.end()
            || !client.connect_to_node(endpoint->second.ip.c_str(), endpoint->second.port)
            || !client.send_filtered_query(shard_req, matches, shard_count)) {
            std::cout << "[Coordinator] Query failed: no answer from Node " << node_id << "\n";
            return false;
        }
        results.insert(results.end(), matches.begin(), matches.end());
        found += shard_count;
    }
    count += found;
    return true;
}