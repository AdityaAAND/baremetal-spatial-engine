#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include "NodeConfig.h"
#include "SpatialRecord.h"

struct NodeEndpoint{
    std::string ip;
    int port;
};

class Coordinator {
private:
    std::vector<ShardRange> cluster_shards;

public:
    Coordinator();
    void register_shard(uint32_t node_id, uint64_t start_key, uint64_t end_key);
    uint32_t route_query(float x, float y, float z);
    bool split_shard(uint32_t old_node_id, uint32_t new_node_id, NodeEndpoint old_node, NodeEndpoint new_node);
    void failover_primary_to_standby(uint32_t failed_node_id, uint32_t standby_node_id);
    bool execute_2pc_transaction(uint64_t tx_id, const SpatialRecord& record, const std::vector<NodeEndpoint>& participants);
    const std::vector<ShardRange>& get_shards() const{return cluster_shards;}

};
