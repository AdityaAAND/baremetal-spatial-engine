#pragma once
#include <cstdint>
#include "SpatialRecord.h"

enum CommandType : uint8_t {
    CMD_INGEST      = 0x01,
    CMD_QUERY       = 0x02,
    CMD_QUERY_RESP  = 0x03,
    CMD_ACK         = 0x04,
    CMD_PREPARE     = 0x05,
    CMD_VOTE_COMMIT = 0x06,
    CMD_VOTE_ABORT  = 0x07,
    CMD_COMMIT      = 0x08,
    CMD_ABORT       = 0x09,
    CMD_REPLICATE   = 0x0A,
    CMD_MIGRATE_RANGE = 0x0B,
    CMD_HEARTBEAT     = 0x0c,
    CMD_SPLIT_SHARD   = 0x0d,
    CMD_QUERY_FILTERED = 0x0e,
    CMD_COUNT_RESP     = 0x0f,
    CMD_GET_KEYS       = 0x10,
    CMD_KEYS_RESP      = 0x11,
    CMD_GET_PENDING_TX  = 0x12,
    CMD_PENDING_TX_RESP = 0x13
};

enum FilterOp : uint8_t {
    FILTER_EQ = 0,
    FILTER_NE,
    FILTER_LT,
    FILTER_LE,
    FILTER_GT,
    FILTER_GE
};

// Fixed so the request stays a single packed struct, like every other payload
constexpr uint8_t MAX_QUERY_FILTERS = 4;

#pragma pack(push, 1)
struct PacketHeader {
    uint8_t command;      // CommandType
    uint32_t payload_len; // Size of binary payload following header
};

struct QueryRequest {
    float x_min, x_max;
    float y_min, y_max;
    float z_min, z_max;
};

struct QueryFilter {
    uint8_t op;     // FilterOp, compared against (SpatialRecord::metadata & mask)
    uint32_t value; // already shifted into the mask's bits
    uint32_t mask;  // which bits of metadata take part: all, the dataset id, or the intensity
};

struct FilteredQueryRequest {
    QueryRequest box;
    uint32_t limit;      // stop after this many matches; 0 means no limit
    uint8_t count_only;  // non-zero replies CMD_COUNT_RESP (uint64_t) instead of rows
    uint8_t filter_count;
    QueryFilter filters[MAX_QUERY_FILTERS]; // ANDed together
};

struct TxPacket {
    uint64_t tx_id;
    SpatialRecord record;
};
struct MigrateRangePayload{
    uint64_t min_key;
    uint64_t max_key;
    char target_ip[16];
    uint32_t target_port;
};
struct SplitShardPayload{
    uint32_t old_node;
    uint32_t new_node;
    uint64_t split_key;
    

};
#pragma pack(pop)