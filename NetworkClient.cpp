#include "NetworkClient.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>

// Upper bound on how long any reply may take before the peer is treated as failed
static constexpr int RECV_TIMEOUT_SEC = 5;

// A write to a closed peer must fail with an error, not kill the process with SIGPIPE.
// Linux takes a per-send flag; macOS uses the SO_NOSIGPIPE socket option set in connect_to_node.
#ifdef MSG_NOSIGNAL
static constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
static constexpr int SEND_FLAGS = 0;
#endif

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

static bool send_packet(int fd, uint8_t command, const void* payload, uint32_t payload_len) {
    PacketHeader header = {command, payload_len};
    return send_all(fd, &header, sizeof(PacketHeader)) && send_all(fd, payload, payload_len);
}

// A timeout can return a partial header, so require the full size
static bool recv_header(int fd, PacketHeader& header) {
    return recv(fd, &header, sizeof(PacketHeader), MSG_WAITALL) == static_cast<ssize_t>(sizeof(PacketHeader));
}

NetworkClient::NetworkClient() : sock_fd(-1) {}

bool NetworkClient::connect_to_node(const char* ip, int port) {
    close_connection();

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

    // Without this, recv() blocks forever on a hung worker
    timeval tv{RECV_TIMEOUT_SEC, 0};
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    setsockopt(sock_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    return true;
}

bool NetworkClient::send_record(const SpatialRecord& record) {
    if (sock_fd < 0) return false;
    return send_packet(sock_fd, CMD_INGEST, &record, sizeof(SpatialRecord));
}

bool NetworkClient::send_query(const QueryRequest& req, std::vector<SpatialRecord>& matches) {
    matches.clear();
    if (sock_fd < 0) return false;
    if (!send_packet(sock_fd, CMD_QUERY, &req, sizeof(QueryRequest))) return false;

    PacketHeader resp_header;
    if (!recv_header(sock_fd, resp_header)) return false;
    const uint32_t payload_len = resp_header.payload_len;
    if (resp_header.command != CMD_QUERY_RESP || payload_len % sizeof(SpatialRecord) != 0) return false;
    if (payload_len == 0) return true;

    matches.resize(payload_len / sizeof(SpatialRecord));
    // A short read would leave zero-filled rows that look like real points at the origin
    if (recv(sock_fd, matches.data(), payload_len, MSG_WAITALL) != static_cast<ssize_t>(payload_len)) {
        matches.clear();
        return false;
    }
    return true;
}

uint8_t NetworkClient::send_prepare(uint64_t tx_id, const SpatialRecord& record) {
    if (sock_fd < 0) return CMD_VOTE_ABORT;

    TxPacket packet = {tx_id, record};
    if (!send_packet(sock_fd, CMD_PREPARE, &packet, sizeof(TxPacket))) return CMD_VOTE_ABORT;

    PacketHeader resp;
    return recv_header(sock_fd, resp) ? resp.command : static_cast<uint8_t>(CMD_VOTE_ABORT);
}

bool NetworkClient::send_commit(uint64_t tx_id) {
    if (sock_fd < 0) return false;
    if (!send_packet(sock_fd, CMD_COMMIT, &tx_id, sizeof(uint64_t))) return false;

    PacketHeader resp;
    return recv_header(sock_fd, resp) && resp.command == CMD_ACK;
}

bool NetworkClient::send_abort(uint64_t tx_id) {
    if (sock_fd < 0) return false;
    if (!send_packet(sock_fd, CMD_ABORT, &tx_id, sizeof(uint64_t))) return false;

    PacketHeader resp;
    return recv_header(sock_fd, resp) && resp.command == CMD_ACK;
}

bool NetworkClient::send_migrate(const MigrateRangePayload& payload) {
    if (sock_fd < 0) return false;
    if (!send_packet(sock_fd, CMD_MIGRATE_RANGE, &payload, sizeof(MigrateRangePayload))) return false;

    // The worker replies CMD_ACK only after the records reached the target
    PacketHeader resp;
    return recv_header(sock_fd, resp) && resp.command == CMD_ACK;
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
