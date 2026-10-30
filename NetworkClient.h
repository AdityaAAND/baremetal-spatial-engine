#pragma once
#include "SpatialRecord.h"
#include "NetworkProtocol.h"
#include <vector>

class NetworkClient {
private:
    int sock_fd;

public:
    NetworkClient();
    bool connect_to_node(const char* ip, int port);
    bool send_record(const SpatialRecord& record);
    std::vector<SpatialRecord> send_query(const QueryRequest& req);
    
    uint8_t send_prepare(uint64_t tx_id, const SpatialRecord& record);
    bool send_commit(uint64_t tx_id);
    bool send_abort(uint64_t tx_id);

    void close_connection();
    ~NetworkClient();
};