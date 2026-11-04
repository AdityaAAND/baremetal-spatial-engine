#include "MortonClient.h"
#include <iostream>

SpatialQueryBuilder::SpatialQueryBuilder(NetworkClient& net_client) 
    : client(net_client), req{0, 0, 0, 0, 0, 0}, bbox_set(false) {}

SpatialQueryBuilder& SpatialQueryBuilder::bounding_box(float x_min, float x_max, float y_min, float y_max, float z_min, float z_max) {
    req = {x_min, x_max, y_min, y_max, z_min, z_max};
    bbox_set = true;
    return *this;
}

std::vector<SpatialRecord> SpatialQueryBuilder::execute() {
    if (!bbox_set) {
        std::cerr << "[MortonClient SDK Error] Bounding box parameters not specified!\n";
        return {};
    }
    return client.send_query(req);
}

MortonClient::MortonClient(const std::string& ip, int port) : server_ip(ip), server_port(port) {}

bool MortonClient::connect() {
    return net_client.connect_to_node(server_ip.c_str(), server_port);
}

void MortonClient::disconnect() {
    net_client.close_connection();
}

bool MortonClient::insert(const SpatialRecord& record) {
    return net_client.send_record(record);
}

bool MortonClient::insert(float x, float y, float z, uint64_t timestamp, uint32_t intensity) {
    SpatialRecord rec = {timestamp, x, y, z, intensity};
    return net_client.send_record(rec);
}

SpatialQueryBuilder MortonClient::query() {
    return SpatialQueryBuilder(net_client);
}