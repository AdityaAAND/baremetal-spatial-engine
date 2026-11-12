#include "VisualizerServer.h"
#include "MortonCode.h"
#include "SpatialRecord.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

static constexpr const char* PAGE_FILE = "visualizer.html";
static constexpr size_t MAX_REQUEST_BYTES = 8192;
// About 15 MB of JSON at most, which a browser still parses quickly
static constexpr uint32_t MAX_POINTS = 500000;
// How often the accept loop looks at the stop flag
static constexpr int STOP_POLL_MS = 200;
static constexpr int REQUEST_TIMEOUT_SEC = 2;
static constexpr int REPLY_TIMEOUT_SEC = 10;

#ifdef MSG_NOSIGNAL
static constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
static constexpr int SEND_FLAGS = 0;
#endif

namespace {

struct BadRequest {
    std::string message;
};

bool send_all(int fd, const std::string& data) {
    const char* cursor = data.data();
    size_t left = data.size();
    while (left > 0) {
        const ssize_t sent = send(fd, cursor, left, SEND_FLAGS);
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR) continue;
            return false;
        }
        cursor += sent;
        left -= static_cast<size_t>(sent);
    }
    return true;
}

const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        default:  return "Internal Server Error";
    }
}

void reply(int fd, int status, const char* content_type, const std::string& body) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n"
        + "Content-Type: " + content_type + "\r\n"
        + "Content-Length: " + std::to_string(body.size()) + "\r\n"
        // The data changes under the page, so a cached reply would show a stale cloud
        + "Cache-Control: no-store\r\n"
        + "Connection: close\r\n";
    if (status == 405) head += "Allow: GET\r\n";
    head += "\r\n";
    send_all(fd, head) && send_all(fd, body);
}

std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

std::string error_json(const std::string& message) {
    return "{\"ok\":false,\"error\":" + json_string(message) + "}";
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hex_value(text[i + 1]) >= 0 && hex_value(text[i + 2]) >= 0) {
            out += static_cast<char>(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2]));
            i += 2;
        } else {
            out += text[i] == '+' ? ' ' : text[i];
        }
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> params;
    std::stringstream stream(query);
    std::string pair;
    while (std::getline(stream, pair, '&')) {
        if (pair.empty()) continue;
        const size_t equals = pair.find('=');
        const std::string name = url_decode(pair.substr(0, equals));
        params[name] = equals == std::string::npos ? "" : url_decode(pair.substr(equals + 1));
    }
    return params;
}

// A missing parameter keeps the fallback; a present one must be a whole, finite number
float float_param(const std::map<std::string, std::string>& params, const char* name, float fallback) {
    const auto found = params.find(name);
    if (found == params.end() || found->second.empty()) return fallback;
    char* end = nullptr;
    const float value = std::strtof(found->second.c_str(), &end);
    if (*end != '\0' || !std::isfinite(value)) throw BadRequest{std::string(name) + " must be a number"};
    return value;
}

bool uint_param(const std::map<std::string, std::string>& params, const char* name, uint32_t low, uint32_t high, uint32_t& value) {
    const auto found = params.find(name);
    if (found == params.end() || found->second.empty()) return false;
    const std::string& text = found->second;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (text.find_first_not_of("0123456789") != std::string::npos || *end != '\0' || errno != 0 || parsed < low || parsed > high) {
        throw BadRequest{std::string(name) + " must be a whole number from " + std::to_string(low) + " to " + std::to_string(high)};
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

const char* plan_name(ExecutionPlanType type) {
    switch (type) {
        case ExecutionPlanType::MORTON_INDEX_RANGE_SCAN: return "MORTON_INDEX_RANGE_SCAN";
        case ExecutionPlanType::MORTON_SPLIT_RANGE_SCAN: return "MORTON_SPLIT_RANGE_SCAN";
        default: return "INVALID_PLAN";
    }
}

void append_number(std::string& out, const char* format, double value) {
    char text[40];
    std::snprintf(text, sizeof(text), format, value);
    out += text;
}

// Key spans go up to 2^63, past what a JavaScript number holds exactly, so they travel as strings
std::string plan_json(const ExecutionPlan& plan) {
    std::string out = "{\"type\":\"" + std::string(plan_name(plan.plan_type)) + "\""
        + ",\"description\":" + json_string(plan.description)
        + ",\"key_span\":\"" + std::to_string(plan.key_span) + "\""
        + ",\"scan_cost\":\"" + std::to_string(plan.scan_cost) + "\""
        + ",\"sub_boxes\":[";
    for (size_t i = 0; i < plan.sub_queries.size(); ++i) {
        const QueryRequest& box = plan.sub_queries[i];
        const float bounds[6] = {box.x_min, box.x_max, box.y_min, box.y_max, box.z_min, box.z_max};
        out += i == 0 ? "[" : ",[";
        for (int b = 0; b < 6; ++b) {
            if (b != 0) out += ',';
            append_number(out, "%.9g", bounds[b]);
        }
        out += ']';
    }
    return out + "]}";
}

} // namespace

VisualizerServer::VisualizerServer(Coordinator& coordinator_ref, MortonClient& client_ref, std::shared_mutex& lock, int port)
    : coordinator(coordinator_ref), client(client_ref), cluster_lock(lock), port(port), server_fd(-1), stopping(false) {}

VisualizerServer::~VisualizerServer() {
    stop();
}

bool VisualizerServer::start() {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return false;

    int on = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    // Loopback only: anyone who can connect can read every loaded point
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(server_fd, 16) != 0) {
        close(server_fd);
        server_fd = -1;
        return false;
    }

    thread = std::thread(&VisualizerServer::serve, this);
    std::cout << "[Visualizer] Listening on http://127.0.0.1:" << port << "/\n";
    return true;
}

void VisualizerServer::stop() {
    stopping = true;
    if (thread.joinable()) thread.join();
    if (server_fd >= 0) close(server_fd);
    server_fd = -1;
}

void VisualizerServer::serve() {
    // Polling rather than blocking in accept, so stop() can end the loop without closing a
    // descriptor another thread is waiting on
    while (!stopping) {
        pollfd waiting{server_fd, POLLIN, 0};
        if (poll(&waiting, 1, STOP_POLL_MS) <= 0) continue;

        const int fd = accept(server_fd, nullptr, nullptr);
        if (fd < 0) continue;

#ifdef SO_NOSIGPIPE
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        // One connection at a time, so a client that stalls must not hold up the rest for long
        const timeval request_timeout{REQUEST_TIMEOUT_SEC, 0};
        const timeval reply_timeout{REPLY_TIMEOUT_SEC, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &request_timeout, sizeof(request_timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &reply_timeout, sizeof(reply_timeout));

        handle(fd);
        close(fd);
    }
}

void VisualizerServer::handle(int fd) {
    std::string request;
    char chunk[2048];
    while (request.find("\r\n\r\n") == std::string::npos) {
        if (request.size() > MAX_REQUEST_BYTES) {
            reply(fd, 400, "application/json", error_json("request is too large"));
            return;
        }
        const ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return; // closed or timed out before a full request: nothing to answer
        request.append(chunk, static_cast<size_t>(got));
    }
    // Checked again here because the last chunk can carry both the excess and the blank line
    if (request.size() > MAX_REQUEST_BYTES) {
        reply(fd, 400, "application/json", error_json("request is too large"));
        return;
    }

    // "GET /path?query HTTP/1.1"
    const std::string line = request.substr(0, request.find("\r\n"));
    const size_t method_end = line.find(' ');
    const size_t target_end = line.rfind(' ');
    if (method_end == std::string::npos || target_end == method_end || line.compare(target_end + 1, 5, "HTTP/") != 0) {
        reply(fd, 400, "application/json", error_json("malformed request line"));
        return;
    }
    const std::string method = line.substr(0, method_end);
    const std::string target = line.substr(method_end + 1, target_end - method_end - 1);
    if (method != "GET") {
        reply(fd, 405, "application/json", error_json("only GET is supported"));
        return;
    }

    const size_t question = target.find('?');
    const std::string path = target.substr(0, question);
    const std::string query = question == std::string::npos ? "" : target.substr(question + 1);

    int status = 200;
    std::string body;
    const char* content_type = "application/json";

    if (path == "/" || path == "/visualizer.html") {
        std::ifstream page(PAGE_FILE, std::ios::binary);
        if (page) {
            body.assign(std::istreambuf_iterator<char>(page), std::istreambuf_iterator<char>());
            content_type = "text/html; charset=utf-8";
        } else {
            status = 404;
            content_type = "text/plain; charset=utf-8";
            body = std::string(PAGE_FILE) + " is not in the directory spatial_db was started from.\n";
        }
    } else if (path == "/api/points" || path == "/api/stats") {
        // Held for the whole query: a rebalance in between would move points the query has yet to read
        std::shared_lock<std::shared_mutex> lock(cluster_lock);
        try {
            body = path == "/api/points" ? points_json(parse_query(query), status) : stats_json();
        } catch (const BadRequest& bad) {
            status = 400;
            body = error_json(bad.message);
        }
    } else {
        status = 404;
        body = error_json("no such path");
    }

    std::cout << "[Visualizer] GET " << target << " -> " << status << " (" << body.size() << " bytes)\n";
    reply(fd, status, content_type, body);
}

std::string VisualizerServer::points_json(const std::map<std::string, std::string>& params, int& status) {
    // An axis that is left out covers its full range, as in the shell's BOX
    constexpr float max = std::numeric_limits<float>::max();
    const float x_min = float_param(params, "xmin", -max), x_max = float_param(params, "xmax", max);
    const float y_min = float_param(params, "ymin", -max), y_max = float_param(params, "ymax", max);
    const float z_min = float_param(params, "zmin", -max), z_max = float_param(params, "zmax", max);
    if (x_min > x_max || y_min > y_max || z_min > z_max) throw BadRequest{"a box minimum is greater than its maximum"};

    SpatialQueryBuilder query = client.query();
    query.bounding_box(x_min, x_max, y_min, y_max, z_min, z_max);

    // Dataset 0 holds what INSERT added, and the viewer shows only what a LOAD ingested
    uint32_t value = 0;
    if (uint_param(params, "dataset", 1, MAX_DATASET_ID, value)) query.where(FILTER_EQ, value << 16, DATASET_MASK);
    else query.where(FILTER_GE, 1u << 16, DATASET_MASK);
    if (uint_param(params, "imin", 0, INTENSITY_MASK, value)) query.where(FILTER_GE, value, INTENSITY_MASK);
    if (uint_param(params, "imax", 0, INTENSITY_MASK, value)) query.where(FILTER_LE, value, INTENSITY_MASK);

    uint32_t limit = MAX_POINTS;
    uint_param(params, "limit", 1, MAX_POINTS, limit);
    // One more than asked for, so a full reply can be told apart from a cut-off one
    query.limit(limit + 1);

    QueryResult result = query.execute();
    if (!result.ok) {
        status = 500;
        return error_json(result.error);
    }

    std::vector<SpatialRecord>& records = result.records;
    const bool truncated = records.size() > limit;
    if (truncated) records.resize(limit);
    // JSON has no way to write NaN or infinity
    records.erase(std::remove_if(records.begin(), records.end(), [](const SpatialRecord& rec) {
        return !std::isfinite(rec.x) || !std::isfinite(rec.y) || !std::isfinite(rec.z);
    }), records.end());

    // Rows arrive grouped by sub-box and shard. Sorting by key makes a row's position its rank
    // along the Z-order curve, which is all the viewer needs to colour by key.
    std::vector<std::pair<uint64_t, uint32_t>> order(records.size());
    for (uint32_t i = 0; i < records.size(); ++i) {
        order[i] = {point_key(records[i].x, records[i].y, records[i].z), i};
    }
    std::sort(order.begin(), order.end());

    std::string out;
    out.reserve(records.size() * 40 + 512);
    out += "{\"ok\":true,\"count\":" + std::to_string(records.size())
        + ",\"truncated\":" + (truncated ? "true" : "false")
        + ",\"plan\":" + plan_json(result.plan)
        + ",\"positions\":[";
    for (size_t i = 0; i < order.size(); ++i) {
        const SpatialRecord& rec = records[order[i].second];
        if (i != 0) out += ',';
        // Millimetres: the index cannot tell two points apart below that
        append_number(out, "%.3f", rec.x);
        out += ',';
        append_number(out, "%.3f", rec.y);
        out += ',';
        append_number(out, "%.3f", rec.z);
    }
    out += "],\"payload\":[";
    for (size_t i = 0; i < order.size(); ++i) {
        if (i != 0) out += ',';
        out += std::to_string(records[order[i].second].metadata);
    }
    out += "]}";
    return out;
}

std::string VisualizerServer::stats_json() {
    const QueryResult loaded = client.query().all().where(FILTER_GE, 1u << 16, DATASET_MASK).execute_count();

    std::string out = "{\"ok\":" + std::string(loaded.ok ? "true" : "false")
        + ",\"loaded\":" + std::to_string(loaded.count) + ",\"nodes\":[";
    bool first = true;
    for (const uint32_t node : coordinator.node_ids()) {
        uint64_t points = 0;
        const bool answered = coordinator.node_point_count(node, points);

        out += first ? "{" : ",{";
        first = false;
        // points counts everything the worker stores, including dataset 0
        out += "\"node\":" + std::to_string(node) + ",\"points\":" + (answered ? std::to_string(points) : "null") + ",\"ranges\":[";
        bool first_range = true;
        for (const ShardRange& shard : coordinator.get_shards()) {
            if (shard.node_id != node) continue;
            out += first_range ? "[" : ",[";
            first_range = false;
            out += "\"" + std::to_string(shard.start_key) + "\",\"" + std::to_string(shard.end_key) + "\"]";
        }
        out += "]}";
    }
    return out + "]}";
}
