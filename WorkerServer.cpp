#include "WorkerServer.h"
#include "NetworkProtocol.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <iostream>

WorkerServer::WorkerServer(int port, WriteAheadLog& wal_ref, BufferPool& pool_ref)
    : server_fd(-1), port(port), wal(wal_ref), pool(pool_ref) {}

void WorkerServer::start_listening() {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 5);

    std::cout << "[Worker Node] Listening on TCP port " << port << "...\n";

    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd >= 0) {
        PacketHeader header;
        SpatialRecord record;

        // Read packet header and binary record payload
        while (recv(client_fd, &header, sizeof(PacketHeader), MSG_WAITALL) > 0) {
            if (header.command == CMD_INGEST) {
                recv(client_fd, &record, sizeof(SpatialRecord), MSG_WAITALL);
                
                // Route directly into Layer 1 engine
                wal.append(record);
                pool.insert(record);
                std::cout << "[Worker Node] Received point over TCP: (" 
                          << record.x << ", " << record.y << ", " << record.z << ")\n";
            }
        }
        close(client_fd);
    }
}

void WorkerServer::stop_server() {
    if (server_fd >= 0) close(server_fd);
}
