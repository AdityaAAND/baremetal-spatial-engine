#include "WorkerServer.h"
#include "NetworkProtocol.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <iostream>

WorkerServer::WorkerServer(int port, WriteAheadLog& wal_ref, BufferPool& pool_ref, SpatialIndex& index_ref)
    : server_fd(-1), port(port), wal(wal_ref), pool(pool_ref), index(index_ref) {}

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

    std::cout << "[Worker Node " << port << "] Listening for 2PC transactions on TCP port " << port << "...\n";

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) break;

        PacketHeader header;
        while (recv(client_fd, &header, sizeof(PacketHeader), MSG_WAITALL) > 0) {
            if (header.command == CMD_INGEST) {
                SpatialRecord record;
                recv(client_fd, &record, sizeof(SpatialRecord), MSG_WAITALL);
                wal.append(record);
                pool.insert(record);
            } 
            else if (header.command == CMD_PREPARE) {
                TxPacket tx_pkt;
                recv(client_fd, &tx_pkt, sizeof(TxPacket), MSG_WAITALL);

                {
                    std::lock_guard<std::mutex> lock(tx_mutex);
                    staged_tx[tx_pkt.tx_id] = tx_pkt.record;
                }

                std::cout << "[Worker " << port << "] Phase 1: Staged Tx " << tx_pkt.tx_id << " -> Voting VOTE_COMMIT\n";
                PacketHeader resp = {CMD_VOTE_COMMIT, 0};
                send(client_fd, &resp, sizeof(PacketHeader), 0);
            }
            else if (header.command == CMD_COMMIT) {
                uint64_t tx_id;
                recv(client_fd, &tx_id, sizeof(uint64_t), MSG_WAITALL);

                std::lock_guard<std::mutex> lock(tx_mutex);
                auto it = staged_tx.find(tx_id);
                if (it != staged_tx.end()) {
                    wal.append(it->second);
                    pool.insert(it->second);
                    staged_tx.erase(it);
                    std::cout << "[Worker " << port << "] Phase 2: Committed Tx " << tx_id << " to local storage & index\n";
                }

                PacketHeader resp = {CMD_ACK, 0};
                send(client_fd, &resp, sizeof(PacketHeader), 0);
            }
            else if (header.command == CMD_ABORT) {
                uint64_t tx_id;
                recv(client_fd, &tx_id, sizeof(uint64_t), MSG_WAITALL);

                std::lock_guard<std::mutex> lock(tx_mutex);
                staged_tx.erase(tx_id);
                std::cout << "[Worker " << port << "] Phase 2: Aborted Tx " << tx_id << " -> Staged data purged\n";

                PacketHeader resp = {CMD_ACK, 0};
                send(client_fd, &resp, sizeof(PacketHeader), 0);
            }
        }
        close(client_fd);
    }
}

void WorkerServer::stop_server() {
    if (server_fd >= 0) close(server_fd);
}