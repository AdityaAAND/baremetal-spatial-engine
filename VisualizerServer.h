#pragma once
#include "Coordinator.h"
#include "MortonClient.h"
#include <atomic>
#include <map>
#include <shared_mutex>
#include <string>
#include <thread>

// A small HTTP/1.1 server that lets a browser read the database as JSON. It only reads: every
// request goes through the SDK like a shell query, and only points a LOAD ingested are returned.
//   GET /            the viewer page (visualizer.html in the working directory)
//   GET /api/points  loaded points in a box, sorted by Z-order key, with the plan that ran
//   GET /api/stats   how many loaded points there are and what each worker holds
// It has no authentication, so it listens on 127.0.0.1 only.
class VisualizerServer {
private:
    Coordinator& coordinator;
    MortonClient& client;
    // Shared for a request, exclusive for REBALANCE, which replaces the shard map a query reads
    std::shared_mutex& cluster_lock;
    int port;
    int server_fd;
    std::atomic<bool> stopping;
    std::thread thread;

    void serve();
    void handle(int fd);
    std::string points_json(const std::map<std::string, std::string>& params, int& status);
    std::string stats_json();

public:
    VisualizerServer(Coordinator& coordinator_ref, MortonClient& client_ref, std::shared_mutex& lock, int port);
    ~VisualizerServer();
    VisualizerServer(const VisualizerServer&) = delete;
    VisualizerServer& operator=(const VisualizerServer&) = delete;

    bool start(); // false if the port cannot be bound
    void stop();  // waits for the request in progress, so the Coordinator can be destroyed afterwards
};
