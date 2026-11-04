#pragma once
#include <string>
#include <vector>
#include <memory>
#include "SpatialRecord.h"
#include "NetworkProtocol.h"
#include "NetworkClient.h"

class SpatialQueryBuilder {
private:
    NetworkClient& client;
    QueryRequest req;
    bool bbox_set;

public:
    explicit SpatialQueryBuilder(NetworkClient& net_client);
    SpatialQueryBuilder& bounding_box(float x_min, float x_max, float y_min, float y_max, float z_min, float z_max);
    std::vector<SpatialRecord> execute();
};

class MortonClient {
private:
    NetworkClient net_client;
    std::string server_ip;
    int server_port;

public:
    MortonClient(const std::string& ip, int port);
    bool connect();
    void disconnect();
    
    bool insert(const SpatialRecord& record);
    bool insert(float x, float y, float z, uint64_t timestamp = 0, uint32_t intensity = 1);
    
    SpatialQueryBuilder query();
};