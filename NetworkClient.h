#pragma once
#include "SpatialRecord.h"

class NetworkClient{
private:
    int sock_fd;
public:
    NetworkClient();
    bool connect_to_node(const char*, int port);
    bool send_record(const SpatialRecord& record);
    void close_connection();
    ~NetworkClient();
};
