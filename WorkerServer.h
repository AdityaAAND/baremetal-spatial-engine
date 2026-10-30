#pragma once
#include "WriteAheadLog.h"
#include "BufferPool.h"

class WorkerServer{
private:
    int server_fd;
    int port;
    WriteAheadLog& wal;
    BufferPool& pool;
public:
    WorkerServer(int port, WriteAheadLog& wal_ref, BufferPool& pool_ref);
    void start_listening();
    void stop_server();

};  