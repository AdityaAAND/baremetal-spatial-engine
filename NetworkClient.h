#pragma once
#include "SpatialRecord.h"
#include "NetworkProtocol.h"
#include <vector>

class NetworkClient {
private:
    int sock_fd;

public:
    NetworkClient();
    // Owns a socket, so a copy would close it out from under the original
    NetworkClient(const NetworkClient&) = delete;
    NetworkClient& operator=(const NetworkClient&) = delete;

    bool connect_to_node(const char* ip, int port);
    bool send_record(const SpatialRecord& record);
    // False means the query failed, which is distinct from a successful query with no matches
    bool send_query(const QueryRequest& req, std::vector<SpatialRecord>& matches);
    
    uint8_t send_prepare(uint64_t tx_id, const SpatialRecord& record);
    bool send_commit(uint64_t tx_id);
    bool send_abort(uint64_t tx_id);
    bool send_migrate(const MigrateRangePayload& payload);

    void close_connection();
    ~NetworkClient();
};
