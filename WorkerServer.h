#pragma once
#include "WriteAheadLog.h"
#include "BufferPool.h"
#include "SpatialIndex.h"
#include <unordered_map>
#include <vector>
#include <mutex>
#include <string>


class WorkerServer {
private:
    int server_fd;
    int port;
    WriteAheadLog& wal;
    BufferPool& pool;
    SpatialIndex& index;
    std::unordered_map<uint64_t, std::vector<SpatialRecord>> staged_tx; // tx_id -> records staged on this shard
    std::mutex tx_mutex;

    bool has_standby;
    std::string standby_ip;
    int standby_port;
    bool recovered;
    void forward_to_standby(const SpatialRecord& record);
    void log_or_warn(const SpatialRecord& record);


public:
    WorkerServer(int port, WriteAheadLog& wal_ref, BufferPool& pool_ref, SpatialIndex& index_ref);
    void set_standby_node(const std::string& ip, int port);
    size_t recover(); // replays the WAL into the index; runs once, automatically, from start_listening
    void start_listening();
    void stop_server();
};