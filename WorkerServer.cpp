#include "WorkerServer.h"
#include "NetworkProtocol.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cerrno>
#include <cstring>

WorkerServer::WorkerServer(int port, WriteAheadLog& wal_ref, BufferPool& pool_ref, SpatialIndex& index_ref)
    : server_fd(-1), port(port), wal(wal_ref), pool(pool_ref), index(index_ref), has_standby(false), standby_port(0), recovered(false) {}

size_t WorkerServer::recover() {
    if (recovered) return 0;
    recovered = true;

    // Rebuild the index from durable entries; PREPAREs with no COMMIT/ABORT are re-staged
    std::unordered_map<uint64_t, std::vector<SpatialRecord>> pending;
    size_t applied = 0;
    wal.for_each([&](const LogEntry& e) {
        switch (e.type) {
            case LOG_DATA:
                index.insert(e.rec);
                ++applied;
                break;
            case LOG_PREPARE:
                pending[e.tx_id].push_back(e.rec);
                break;
            case LOG_COMMIT: {
                auto it = pending.find(e.tx_id);
                if (it != pending.end()) {
                    for (const auto& rec : it->second) index.insert(rec);
                    applied += it->second.size();
                    pending.erase(it);
                }
                break;
            }
            case LOG_ABORT:
                pending.erase(e.tx_id);
                break;
        }
    });

    {
        std::lock_guard<std::mutex> lock(tx_mutex);
        for (auto& entry : pending) staged_tx[entry.first] = std::move(entry.second);
    }

    std::cout << "[Worker Node " << port << "] Recovery: replayed " << wal.size() << " WAL entries -> "
              << applied << " records restored, " << pending.size() << " in-flight tx re-staged\n";
    return applied;
}

void WorkerServer::log_or_warn(const SpatialRecord& record) {
    if (!wal.append(record)) {
        std::cerr << "[Worker Node " << port << "] WAL full: record " << record.metadata << " is not durable\n";
    }
}

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
    recover();

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    // Serving without the bind would leave clients talking to whatever does own this port
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0 || listen(server_fd, 5) < 0) {
        std::cerr << "[Worker Node " << port << "] Cannot listen on TCP port " << port << ": " << strerror(errno) << "\n";
        close(server_fd);
        server_fd = -1;
        return;
    }

    std::cout << "[Worker Node " << port << "] Listening on TCP port " << port << "...\n";

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) break;

        PacketHeader header;
        while (recv(client_fd, &header, sizeof(PacketHeader), MSG_WAITALL) > 0) {
            if (header.command == CMD_INGEST) {
                SpatialRecord record;
                recv(client_fd, &record, sizeof(SpatialRecord), MSG_WAITALL);

                log_or_warn(record);
                pool.insert(record);
                forward_to_standby(record);
            }
            else if (header.command == CMD_REPLICATE) {
                SpatialRecord record;
                recv(client_fd, &record, sizeof(SpatialRecord), MSG_WAITALL);

                log_or_warn(record);
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
                        staged_tx[tx_pkt.tx_id].push_back(tx_pkt.record);
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
                    for (const auto& rec : it->second) {
                        log_or_warn(rec);
                        pool.insert(rec);
                        forward_to_standby(rec);
                    }
                    std::cout << "[Worker " << port << "] Phase 2: Committed Tx " << tx_id << " (" << it->second.size() << " record(s))\n";
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
                bool migrated_ok = true;

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
                        migrated_ok = false;
                        std::cout << "[Worker Node " << port << "] Migration failed: cannot reach "
                                  << target_ip << ":" << payload.target_port << " -> restored "
                                  << records_to_migrate.size() << " records locally\n";
                    }
                    if (dest_sock >= 0) close(dest_sock);
                }

                // CMD_ABORT acts as a NACK so the Coordinator leaves its shard map unchanged
                PacketHeader ack = {static_cast<uint8_t>(migrated_ok ? CMD_ACK : CMD_ABORT), 0};
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