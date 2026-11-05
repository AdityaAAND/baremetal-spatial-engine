#include "WorkerServer.h"
#include "NetworkProtocol.h"
#include "MortonCode.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cerrno>
#include <cstring>
#include <sys/time.h>

// Long enough for the target to apply a whole shard, which it does before answering the heartbeat
static constexpr int MIGRATE_ACK_TIMEOUT_SEC = 30;

#ifdef MSG_NOSIGNAL
static constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
static constexpr int SEND_FLAGS = 0;
#endif

// A peer that has gone must produce a failed send, not kill the whole process with SIGPIPE
static void suppress_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

static bool send_all(int fd, const void* data, size_t len) {
    const char* cursor = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t sent = send(fd, cursor, len, SEND_FLAGS);
        if (sent <= 0) return false;
        cursor += sent;
        len -= static_cast<size_t>(sent);
    }
    return true;
}

// True only once the target has applied every record. CMD_INGEST has no reply, but a worker handles
// a connection in order, so its ACK to the trailing heartbeat proves the records before it went in.
static bool send_records_to(const char* ip, int target_port, const std::vector<SpatialRecord>& records) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    suppress_sigpipe(sock);

    timeval timeout{MIGRATE_ACK_TIMEOUT_SEC, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(target_port));
    inet_pton(AF_INET, ip, &addr.sin_addr);

    // One buffer rather than a send per field, which would stall on Nagle for every small write
    const PacketHeader ingest = {CMD_INGEST, sizeof(SpatialRecord)};
    const PacketHeader heartbeat = {CMD_HEARTBEAT, 0};
    std::vector<char> buffer;
    buffer.reserve(records.size() * (sizeof(ingest) + sizeof(SpatialRecord)) + sizeof(heartbeat));
    auto append = [&](const void* data, size_t len) {
        const char* bytes = static_cast<const char*>(data);
        buffer.insert(buffer.end(), bytes, bytes + len);
    };
    for (const auto& rec : records) {
        append(&ingest, sizeof(ingest));
        append(&rec, sizeof(rec));
    }
    append(&heartbeat, sizeof(heartbeat));

    PacketHeader ack{};
    const bool confirmed = connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0
        && send_all(sock, buffer.data(), buffer.size())
        && recv(sock, &ack, sizeof(ack), MSG_WAITALL) == static_cast<ssize_t>(sizeof(ack))
        && ack.command == CMD_ACK;
    close(sock);
    return confirmed;
}

static bool passes_filters(const SpatialRecord& rec, const FilteredQueryRequest& req) {
    for (uint8_t i = 0; i < req.filter_count; ++i) {
        const uint32_t value = req.filters[i].value;
        bool passed = false;
        switch (req.filters[i].op) {
            case FILTER_EQ: passed = rec.metadata == value; break;
            case FILTER_NE: passed = rec.metadata != value; break;
            case FILTER_LT: passed = rec.metadata < value; break;
            case FILTER_LE: passed = rec.metadata <= value; break;
            case FILTER_GT: passed = rec.metadata > value; break;
            case FILTER_GE: passed = rec.metadata >= value; break;
        }
        if (!passed) return false;
    }
    return true;
}

static bool valid_filtered_query(const FilteredQueryRequest& req) {
    if (req.filter_count > MAX_QUERY_FILTERS) return false;
    for (uint8_t i = 0; i < req.filter_count; ++i) {
        if (req.filters[i].op > FILTER_GE) return false;
    }
    return true;
}

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
            case LOG_REMOVE_RANGE:
                applied -= index.extract_and_remove_range(e.tx_id, e.rec.timestamp).size();
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
        suppress_sigpipe(client_fd);

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

                const SpatialRecord& rec = tx_pkt.record;
                if (!coord_in_range(rec.x) || !coord_in_range(rec.y) || !coord_in_range(rec.z)) {
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
                    migrated_ok = send_records_to(target_ip, static_cast<int>(payload.target_port), records_to_migrate);
                    if (migrated_ok) {
                        if (!wal.append_remove_range(payload.min_key, payload.max_key)) {
                            std::cerr << "[Worker Node " << port << "] WAL full: migrated records will reappear here after a restart\n";
                        }
                        std::cout << "[Worker Node " << port << "] Successfully migrated "
                                  << records_to_migrate.size() << " records to Node " << payload.target_port << "\n";
                    } else {
                        // The target may hold some of them, but only this copy is known to be complete
                        for (const auto& rec : records_to_migrate) index.insert(rec);
                        std::cout << "[Worker Node " << port << "] Migration failed: no confirmation from "
                                  << target_ip << ":" << payload.target_port << " -> restored "
                                  << records_to_migrate.size() << " records locally\n";
                    }
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
            else if (header.command == CMD_HEARTBEAT) {
                PacketHeader resp = {CMD_ACK, 0};
                if (!send_all(client_fd, &resp, sizeof(resp))) break;
            }
            else if (header.command == CMD_GET_KEYS) {
                const std::vector<uint64_t> keys = index.keys();
                const uint32_t payload_len = static_cast<uint32_t>(keys.size() * sizeof(uint64_t));
                PacketHeader resp = {CMD_KEYS_RESP, payload_len};
                if (!send_all(client_fd, &resp, sizeof(resp))) break;
                if (payload_len > 0 && !send_all(client_fd, keys.data(), payload_len)) break;
            }
            else if (header.command == CMD_QUERY_FILTERED) {
                FilteredQueryRequest req;
                // Closing is the failure reply: the client treats a dropped connection as a failed query
                if (recv(client_fd, &req, sizeof(req), MSG_WAITALL) != static_cast<ssize_t>(sizeof(req))
                    || !valid_filtered_query(req)) {
                    break;
                }

                std::vector<SpatialRecord> results;
                uint64_t matched = 0;
                const QueryRequest& box = req.box;
                index.scan_box(box.x_min, box.x_max, box.y_min, box.y_max, box.z_min, box.z_max, [&](const SpatialRecord& rec) {
                    if (!passes_filters(rec, req)) return true;
                    ++matched;
                    if (!req.count_only) results.push_back(rec);
                    return req.limit == 0 || matched < req.limit;
                });

                const void* payload = req.count_only ? static_cast<const void*>(&matched) : results.data();
                const uint32_t payload_len = req.count_only ? static_cast<uint32_t>(sizeof(matched))
                                                            : static_cast<uint32_t>(results.size() * sizeof(SpatialRecord));
                PacketHeader resp_header = {static_cast<uint8_t>(req.count_only ? CMD_COUNT_RESP : CMD_QUERY_RESP), payload_len};
                if (send(client_fd, &resp_header, sizeof(PacketHeader), 0) != static_cast<ssize_t>(sizeof(PacketHeader))) break;
                if (payload_len > 0 && send(client_fd, payload, payload_len, 0) != static_cast<ssize_t>(payload_len)) break;
            }
        }
        close(client_fd);
    }
}

void WorkerServer::stop_server() {
    if (server_fd >= 0) close(server_fd);
}