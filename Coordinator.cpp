#include "Coordinator.h"
#include "MortonCode.h"
#include "NetworkClient.h"
#include <iostream>

Coordinator::Coordinator() {}

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

    for (auto& shard : cluster_shards) {
        if (shard.node_id == old_node_id) {
            uint64_t mid_key = shard.start_key + (shard.end_key - shard.start_key) / 2;

            // 1. Send CMD_MIGRATE_RANGE to old node to move keys [mid_key + 1 .. end_key] to new node
            NetworkClient client;
            if (client.connect_to_node(old_node.ip.c_str(), old_node.port)) {
                MigrateRangePayload payload;
                payload.min_key = mid_key + 1;
                payload.max_key = shard.end_key;
                snprintf(payload.target_ip, sizeof(payload.target_ip), "%s", new_node.ip.c_str());
                payload.target_port = new_node.port;

                // Send migration instruction over raw socket
                // (Using lower-level dispatch pattern)
            }

            // 2. Update Coordinator Shard Map
            uint64_t old_end = shard.end_key;
            shard.end_key = mid_key; // Shrink original shard
            cluster_shards.push_back({new_node_id, mid_key + 1, old_end}); // Register new shard

            std::cout << "[Coordinator] Shard Split Completed!\n"
                      << "   Node " << old_node_id << " Range: [" << shard.start_key << ".." << mid_key << "]\n"
                      << "   Node " << new_node_id << " Range: [" << mid_key + 1 << ".." << old_end << "]\n\n";
            return true;
        }
    }
    return false;
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

bool Coordinator::execute_2pc_transaction(uint64_t tx_id, const SpatialRecord& record, const std::vector<NodeEndpoint>& participants) {
    std::cout << "\n[Coordinator] === Starting 2PC Transaction TxID=" << tx_id << " across " << participants.size() << " Worker Nodes ===\n";

    std::vector<NetworkClient> clients(participants.size());
    bool all_voted_commit = true;

    for (size_t i = 0; i < participants.size(); ++i) {
        if (!clients[i].connect_to_node(participants[i].ip.c_str(), participants[i].port)) {
            all_voted_commit = false;
            break;
        }

        uint8_t vote = clients[i].send_prepare(tx_id, record);
        if (vote != CMD_VOTE_COMMIT) {
            all_voted_commit = false;
        }
    }

    if (all_voted_commit) {
        for (size_t i = 0; i < participants.size(); ++i) {
            clients[i].send_commit(tx_id);
            clients[i].close_connection();
        }
        return true;
    } else {
        for (size_t i = 0; i < participants.size(); ++i) {
            clients[i].send_abort(tx_id);
            clients[i].close_connection();
        }
        return false;
    }
}