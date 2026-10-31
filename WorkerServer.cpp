#include "WorkerServer.h"
#include "NetworkProtocol.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cstring>

WorkerServer::WorkerServer(int port, WriteAheadLog& wal_ref, BufferPool& pool_ref, SpatialIndex& index_ref)
    : server_fd(-1), port(port), wal(wal_ref), pool(pool_ref), index(index_ref), has_standby(false), standby_port(0) {}

void WorkerServer::set_standby_node(const std::string& ip, int port) {
    standby_ip = ip;
    standby_port = port;
    has_standby = true;
}

void WorkerServer::forward_to_standby(const SpatialRecord& record) {
    if (!has_standby) return;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(standby_port);
    inet_pton(AF_INET, standby_ip.c_str(), &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        PacketHeader header = {CMD_REPLICATE, sizeof(SpatialRecord)};
        send(sock, &header, sizeof(PacketHeader), 0);
        send(sock, &record, sizeof(SpatialRecord), 0);
        std::cout << "[Primary Worker " << port << "] Replicated record to Standby Worker " << standby_port << "\n";
    }
    close(sock);
}

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

    std::cout << "[Worker Node " << port << "] Listening on TCP port " << port << "...\n";

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
                forward_to_standby(record);
            }
            else if (header.command == CMD_REPLICATE) {
                SpatialRecord record;
                recv(client_fd, &record, sizeof(SpatialRecord), MSG_WAITALL);
                
                wal.append(record);
                pool.insert(record);
                std::cout << "[Standby Worker " << port << "] Ingested replicated record into local WAL & index\n";
            }
            else if (header.command == CMD_PREPARE) {
                TxPacket tx_pkt;
                recv(client_fd, &tx_pkt, sizeof(TxPacket), MSG_WAITALL);

                if (tx_pkt.record.x < 0.0f) {
                    PacketHeader resp = {CMD_VOTE_ABORT, 0};
                    send(client_fd, &resp, sizeof(PacketHeader), 0);
                } else {
                    {
                        std::lock_guard<std::mutex> lock(tx_mutex);
                        staged_tx[tx_pkt.tx_id] = tx_pkt.record;
                    }
                    PacketHeader resp = {CMD_VOTE_COMMIT, 0};
                    send(client_fd, &resp, sizeof(PacketHeader), 0);
                }
            }
            else if (header.command == CMD_COMMIT) {
                uint64_t tx_id;
                recv(client_fd, &tx_id, sizeof(uint64_t), MSG_WAITALL);

                std::lock_guard<std::mutex> lock(tx_mutex);
                auto it = staged_tx.find(tx_id);
                if (it != staged_tx.end()) {
                    wal.append(it->second);
                    pool.insert(it->second);
                    forward_to_standby(it->second);
                    staged_tx.erase(it);
                }

                PacketHeader resp = {CMD_ACK, 0};
                send(client_fd, &resp, sizeof(PacketHeader), 0);
            }
            else if (header.command == CMD_ABORT) {
                uint64_t tx_id;
                recv(client_fd, &tx_id, sizeof(uint64_t), MSG_WAITALL);

                std::lock_guard<std::mutex> lock(tx_mutex);
                staged_tx.erase(tx_id);

                PacketHeader resp = {CMD_ACK, 0};
                send(client_fd, &resp, sizeof(PacketHeader), 0);
            }
            else if (header.command == CMD_MIGRATE_RANGE) {
                MigrateRangePayload payload;
                recv(client_fd, &payload, sizeof(MigrateRangePayload), MSG_WAITALL);

                // Guarantee NUL termination of the fixed-size IP field from the wire
                char target_ip[sizeof(payload.target_ip)];
                memcpy(target_ip, payload.target_ip, sizeof(target_ip));
                target_ip[sizeof(target_ip) - 1] = '\0';

                std::cout << "[Worker Node " << port << "] Executing Range Migration for keys ["
                          << payload.min_key << ".." << payload.max_key << "] -> Target: "
                          << target_ip << ":" << payload.target_port << "\n";

                std::vector<SpatialRecord> records_to_migrate = index.extract_and_remove_range(payload.min_key, payload.max_key);

                if (!records_to_migrate.empty()) {
                    int dest_sock = socket(AF_INET, SOCK_STREAM, 0);
                    sockaddr_in dest_addr{};
                    dest_addr.sin_family = AF_INET;
                    dest_addr.sin_port = htons(static_cast<uint16_t>(payload.target_port));
                    inet_pton(AF_INET, target_ip, &dest_addr.sin_addr);

                    if (dest_sock >= 0 && connect(dest_sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr)) == 0) {
                        for (const auto& rec : records_to_migrate) {
                            PacketHeader ingest_header = {CMD_INGEST, sizeof(SpatialRecord)};
                            send(dest_sock, &ingest_header, sizeof(PacketHeader), 0);
                            send(dest_sock, &rec, sizeof(SpatialRecord), 0);
                        }
                        std::cout << "[Worker Node " << port << "] Successfully migrated "
                                  << records_to_migrate.size() << " records to Node " << payload.target_port << "\n";
                    } else {
                        // Target unreachable: put the records back so they are not lost
                        for (const auto& rec : records_to_migrate) index.insert(rec);
                        std::cout << "[Worker Node " << port << "] Migration failed: cannot reach "
                                  << target_ip << ":" << payload.target_port << " -> restored "
                                  << records_to_migrate.size() << " records locally\n";
                    }
                    if (dest_sock >= 0) close(dest_sock);
                }

                PacketHeader ack = {CMD_ACK, 0};
                send(client_fd, &ack, sizeof(PacketHeader), 0);
            }
            else if (header.command == CMD_QUERY) {
                QueryRequest req;
                recv(client_fd, &req, sizeof(QueryRequest), MSG_WAITALL);

                std::vector<SpatialRecord> results = index.range_query(req.x_min, req.x_max, req.y_min, req.y_max, req.z_min, req.z_max);
                PacketHeader resp_header = {CMD_QUERY_RESP, static_cast<uint32_t>(results.size() * sizeof(SpatialRecord))};
                send(client_fd, &resp_header, sizeof(PacketHeader), 0);
                if (!results.empty()) {
                    send(client_fd, results.data(), resp_header.payload_len, 0);
                }
            }
        }
        close(client_fd);
    }
}

void WorkerServer::stop_server() {
    if (server_fd >= 0) close(server_fd);
}