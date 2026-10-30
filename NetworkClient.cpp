#include "NetworkClient.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

NetworkClient::NetworkClient() : sock_fd(-1) {}

bool NetworkClient::connect_to_node(const char* ip, int port) {
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) return false;

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &server_addr.sin_addr);

    if (connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sock_fd);
        sock_fd = -1;
        return false;
    }
    return true;
}

bool NetworkClient::send_record(const SpatialRecord& record) {
    if (sock_fd < 0) return false;
    PacketHeader header = {CMD_INGEST, sizeof(SpatialRecord)};
    send(sock_fd, &header, sizeof(PacketHeader), 0);
    send(sock_fd, &record, sizeof(SpatialRecord), 0);
    return true;
}

std::vector<SpatialRecord> NetworkClient::send_query(const QueryRequest& req) {
    std::vector<SpatialRecord> matches;
    if (sock_fd < 0) return matches;

    PacketHeader header = {CMD_QUERY, sizeof(QueryRequest)};
    send(sock_fd, &header, sizeof(PacketHeader), 0);
    send(sock_fd, &req, sizeof(QueryRequest), 0);

    PacketHeader resp_header;
    if (recv(sock_fd, &resp_header, sizeof(PacketHeader), MSG_WAITALL) > 0) {
        if (resp_header.command == CMD_QUERY_RESP && resp_header.payload_len > 0) {
            size_t count = resp_header.payload_len / sizeof(SpatialRecord);
            matches.resize(count);
            recv(sock_fd, matches.data(), resp_header.payload_len, MSG_WAITALL);
        }
    }
    return matches;
}

uint8_t NetworkClient::send_prepare(uint64_t tx_id, const SpatialRecord& record) {
    if (sock_fd < 0) return CMD_VOTE_ABORT;

    TxPacket packet = {tx_id, record};
    PacketHeader header = {CMD_PREPARE, sizeof(TxPacket)};
    send(sock_fd, &header, sizeof(PacketHeader), 0);
    send(sock_fd, &packet, sizeof(TxPacket), 0);

    PacketHeader resp;
    if (recv(sock_fd, &resp, sizeof(PacketHeader), MSG_WAITALL) > 0) {
        return resp.command;
    }
    return CMD_VOTE_ABORT;
}

bool NetworkClient::send_commit(uint64_t tx_id) {
    if (sock_fd < 0) return false;

    PacketHeader header = {CMD_COMMIT, sizeof(uint64_t)};
    send(sock_fd, &header, sizeof(PacketHeader), 0);
    send(sock_fd, &tx_id, sizeof(uint64_t), 0);

    PacketHeader resp;
    return (recv(sock_fd, &resp, sizeof(PacketHeader), MSG_WAITALL) > 0 && resp.command == CMD_ACK);
}

bool NetworkClient::send_abort(uint64_t tx_id) {
    if (sock_fd < 0) return false;

    PacketHeader header = {CMD_ABORT, sizeof(uint64_t)};
    send(sock_fd, &header, sizeof(PacketHeader), 0);
    send(sock_fd, &tx_id, sizeof(uint64_t), 0);

    PacketHeader resp;
    return (recv(sock_fd, &resp, sizeof(PacketHeader), MSG_WAITALL) > 0 && resp.command == CMD_ACK);
}

void NetworkClient::close_connection() {
    if (sock_fd >= 0) {
        close(sock_fd);
        sock_fd = -1;
    }
}

NetworkClient::~NetworkClient() {
    close_connection();
}