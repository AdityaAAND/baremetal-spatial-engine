#include "Coordinator.h"
#include "MortonCode.h"
#include "NetworkClient.h"
#include <cmath>
#include <iostream>

Coordinator::Coordinator() {}

void Coordinator::register_shard(uint32_t node_id, uint64_t start_key, uint64_t end_key) {
    cluster_shards.push_back({node_id, start_key, end_key});
}

uint32_t Coordinator::route_query(float x, float y, float z) {
    uint32_t x_int = static_cast<uint32_t>(std::abs(x * 1000.0f));
    uint32_t y_int = static_cast<uint32_t>(std::abs(y * 1000.0f));
    uint32_t z_int = static_cast<uint32_t>(std::abs(z * 1000.0f));

    uint64_t target_key = calculate_z_order(x_int, y_int, z_int);

    for (const auto& shard : cluster_shards) {
        if (target_key >= shard.start_key && target_key <= shard.end_key) {
            return shard.node_id;
        }
    }
    return 0;
}

bool Coordinator::execute_2pc_transaction(uint64_t tx_id, const SpatialRecord& record, const std::vector<NodeEndpoint>& participants) {
    std::cout << "\n[Coordinator] === Starting 2PC Transaction TxID=" << tx_id << " across " << participants.size() << " Worker Nodes ===\n";

    std::vector<NetworkClient> clients(participants.size());
    bool all_voted_commit = true;

    // --- PHASE 1: PREPARE ---
    std::cout << "[Coordinator] Phase 1: Sending PREPARE to all participants...\n";
    for (size_t i = 0; i < participants.size(); ++i) {
        if (!clients[i].connect_to_node(participants[i].ip.c_str(), participants[i].port)) {
            std::cout << "[Coordinator] Phase 1 Error: Failed to connect to worker " << participants[i].port << "\n";
            all_voted_commit = false;
            break;
        }

        uint8_t vote = clients[i].send_prepare(tx_id, record);
        if (vote != CMD_VOTE_COMMIT) {
            std::cout << "[Coordinator] Phase 1: Worker " << participants[i].port << " voted VOTE_ABORT\n";
            all_voted_commit = false;
        } else {
            std::cout << "[Coordinator] Phase 1: Worker " << participants[i].port << " voted VOTE_COMMIT\n";
        }
    }

    // --- PHASE 2: COMMIT OR ABORT ---
    if (all_voted_commit) {
        std::cout << "[Coordinator] Phase 2: All nodes voted VOTE_COMMIT -> Dispatching GLOBAL_COMMIT\n";
        for (size_t i = 0; i < participants.size(); ++i) {
            clients[i].send_commit(tx_id);
            clients[i].close_connection();
        }
        std::cout << "[Coordinator] === TxID=" << tx_id << " Transaction COMMITTED Successfully ===\n\n";
        return true;
    } else {
        std::cout << "[Coordinator] Phase 2: Voting failed -> Dispatching GLOBAL_ABORT\n";
        for (size_t i = 0; i < participants.size(); ++i) {
            clients[i].send_abort(tx_id);
            clients[i].close_connection();
        }
        std::cout << "[Coordinator] === TxID=" << tx_id << " Transaction ABORTED Safely ===\n\n";
        return false;
    }
}