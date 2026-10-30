#include "NetworkClient.h"
#include "NetworkProtocol.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

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

    // Send Header followed by raw binary SpatialRecord
    send(sock_fd, &header, sizeof(PacketHeader), 0);
    send(sock_fd, &record, sizeof(SpatialRecord), 0);
    return true;
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