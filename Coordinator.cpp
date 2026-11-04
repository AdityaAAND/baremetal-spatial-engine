#include "Coordinator.h"
#include "MortonCode.h"
#include "NetworkClient.h"
#include <iostream>
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

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

uint64_t Coordinator::next_tx_id() {
    return ++last_tx_id;
}

bool Coordinator::execute_query(const QueryRequest& box, std::vector<SpatialRecord>& results) {
    // Every point in the box has a key between the corner keys, so other shards cannot hold matches
    const uint64_t min_key = point_key(box.x_min, box.y_min, box.z_min);
    const uint64_t max_key = point_key(box.x_max, box.y_max, box.z_max);

    // A set because a node owns several ranges after a split, and must only be asked once
    std::set<uint32_t> nodes;
    for (const auto& shard : cluster_shards) {
        if (shard.start_key <= max_key && shard.end_key >= min_key) nodes.insert(shard.node_id);
    }

    for (uint32_t node_id : nodes) {
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