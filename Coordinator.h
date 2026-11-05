#pragma once
#include <set>
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

struct RebalanceResult {
    bool ok;
    std::string error;
    uint64_t total_points;
    uint64_t moved_points;
};

class Coordinator {
private:
    std::vector<ShardRange> cluster_shards;
    std::unordered_map<uint32_t, NodeEndpoint> node_endpoints; // node_id -> network address
    uint64_t last_tx_id;

    // Every point in the box has a key between the corner keys, so other shards cannot hold matches
    std::set<uint32_t> overlapping_nodes(const QueryRequest& box) const;

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
    // Runs filters and the limit on the workers. Appends rows and adds to count; false if any shard failed.
    bool execute_filtered_query(const FilteredQueryRequest& req, std::vector<SpatialRecord>& results, uint64_t& count);
    const std::vector<ShardRange>& get_shards() const{return cluster_shards;}

    // Registered nodes in ascending order, including any that own no keys yet
    std::vector<uint32_t> node_ids() const;
    bool node_point_count(uint32_t node_id, uint64_t& count);

    // Moves points so every registered node owns an equal share, then saves the new shard map to map_path.
    // On failure the shard map is unchanged but some points may have moved; running it again repairs that.
    RebalanceResult rebalance(const std::string& map_path);

    bool save_shard_map(const std::string& path) const;
    bool load_shard_map(const std::string& path); // false if there is no valid file; the map is then unchanged

};
