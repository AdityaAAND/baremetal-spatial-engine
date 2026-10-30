#pragma once 
#include <cstdint>
#include "SpatialRecord.h"

enum CommandType : uint8_t{
    CMD_INGEST = 0x01,
    CMD_QUERY = 0x02,
    CMD_ACK = 0x03

};

#pragma pack(push, 1)
struct PacketHeader{
    uint8_t command;
    uint32_t payload_len;

};
#pragma pack(pop)
