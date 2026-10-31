#pragma once
#include "WriteAheadLog.h"
#include "BufferPool.h"
#include "SpatialIndex.h"
#include <unordered_map>
#include <mutex>
#include <string>


class WorkerServer {
private:
    int server_fd;
    int port;
    WriteAheadLog& wal;
    BufferPool& pool;
    SpatialIndex& index;
    std::unordered_map<uint64_t, SpatialRecord> staged_tx;
    std::mutex tx_mutex;

    bool has_standby;
    std::string standby_ip;
    int standby_port;
    void forward_to_standby(const SpatialRecord& record);


public:
    WorkerServer(int port, WriteAheadLog& wal_ref, BufferPool& pool_ref, SpatialIndex& index_ref);
    void set_standby_node(const std::string& ip, int port);
    void start_listening();
    void stop_server();
};