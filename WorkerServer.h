#pragma once
#include "WriteAheadLog.h"
#include "BufferPool.h"
#include "SpatialIndex.h"
#include <unordered_map>
#include <mutex>


class WorkerServer {
private:
    int server_fd;
    int port;
    WriteAheadLog& wal;
    BufferPool& pool;
    SpatialIndex& index;
    std::unordered_map<uint64_t, SpatialRecord> staged_tx;
    std::mutex tx_mutex;

public:
    WorkerServer(int port, WriteAheadLog& wal_ref, BufferPool& pool_ref, SpatialIndex& index_ref);
    void start_listening();
    void stop_server();
};