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
    CMD_SPLIT_SHARD   = 0x0d
};

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

};
#pragma pack(pop)