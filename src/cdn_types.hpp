#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct Endpoint {
    std::string host;
    int port = 0;
};

struct Config {
    std::string mode = "edge";
    std::string listen = "0.0.0.0";
    int port = 8080;
    int cache_seconds = 86400;
    int timeout_ms = 5000;
    int max_connections = 256;
    int worker_threads = 8;
    int health_interval_ms = 2000;
    int health_failure_threshold = 2;
    int health_recovery_threshold = 2;
    std::filesystem::path root = "public";
    std::string region = "local";
    std::string default_region = "north";
    std::string trusted_proxy = "127.0.0.1/32";
    std::string geoip_database;
    bool development_override = false;
    std::string router_token;
    std::string metrics_token;
    std::map<std::string, Endpoint> edges;
};

struct Request {
    std::string method;
    std::string target;
    std::string version;
    std::vector<std::pair<std::string, std::string>> headers;
};
