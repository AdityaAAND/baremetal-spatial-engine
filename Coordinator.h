#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <unordered_map>
#include "NodeConfig.h"
#include "SpatialRecord.h"
#include "NetworkProtocol.h"

struct NodeEndpoint{
    std::string ip;
    int port;
};

class Coordinator {
private:
    std::vector<ShardRange> cluster_shards;
    std::unordered_map<uint32_t, NodeEndpoint> node_endpoints; // node_id -> network address
    uint64_t last_tx_id;

public:
    Coordinator();
    void register_node(uint32_t node_id, const NodeEndpoint& endpoint);
    void register_shard(uint32_t node_id, uint64_t start_key, uint64_t end_key);
    uint32_t route_query(float x, float y, float z);
    bool split_shard(uint32_t old_node_id, uint32_t new_node_id, NodeEndpoint old_node, NodeEndpoint new_node);
    void failover_primary_to_standby(uint32_t failed_node_id, uint32_t standby_node_id);
    // Atomically writes a batch: each record is sent only to the shard that owns its Z-order key
    bool execute_2pc_transaction(uint64_t tx_id, const std::vector<SpatialRecord>& records);
    uint64_t next_tx_id();
    // Appends the matches from every shard overlapping the box; false if any shard failed to answer
    bool execute_query(const QueryRequest& box, std::vector<SpatialRecord>& results);
    const std::vector<ShardRange>& get_shards() const{return cluster_shards;}

};
