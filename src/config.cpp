#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace {
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool parse_int(const std::string& text, int minimum, int maximum, int& result) {
    try {
        size_t used = 0;
        const long value = std::stol(text, &used);
        if (used != text.size() || value < minimum || value > maximum) return false;
        result = static_cast<int>(value);
        return true;
    } catch (...) {
        return false;
    }
}

std::string unquote(std::string value) {
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool parse_url(const std::string& url, Endpoint& endpoint) {
    constexpr const char* prefix = "http://";
    if (url.rfind(prefix, 0) != 0) return false;
    const std::string authority = url.substr(7);
    if (authority.empty() || authority.find('/') != std::string::npos) return false;
    const auto colon = authority.rfind(':');
    endpoint.host = colon == std::string::npos ? authority : authority.substr(0, colon);
    endpoint.port = 80;
    return !endpoint.host.empty() &&
           (colon == std::string::npos || parse_int(authority.substr(colon + 1), 1, 65535, endpoint.port));
}
}  // namespace

bool load_config(const std::filesystem::path& path, Config& config, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "Cannot open config: " + path.string();
        return false;
    }

    std::string section;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            error = "Invalid config line " + std::to_string(line_number);
            return false;
        }
        const std::string key = trim(line.substr(0, equals));
        const std::string value = unquote(line.substr(equals + 1));

        if (section == "router") {
            if (key == "listen") {
                const auto colon = value.rfind(':');
                if (colon == std::string::npos || !parse_int(value.substr(colon + 1), 1, 65535, config.port)) {
                    error = "Invalid router.listen";
                    return false;
                }
                config.listen = value.substr(0, colon);
            } else if (key == "default_region") config.default_region = value;
            else if (key == "geoip_database") config.geoip_database = value;
            else if (key == "trust_proxy") config.trusted_proxy = value;
            else if (key == "development_allow_client_ip_override") config.development_override = lower(value) == "true";
            else if (key == "router_token") config.router_token = value;
            else if (key == "timeout_ms" && !parse_int(value, 100, 60000, config.timeout_ms)) error = "Invalid timeout_ms";
            else if (key == "max_connections" && !parse_int(value, 1, 10000, config.max_connections)) error = "Invalid max_connections";
            else if (key == "worker_threads" && !parse_int(value, 1, 256, config.worker_threads)) error = "Invalid worker_threads";
            else if (key == "health_interval_ms" && !parse_int(value, 100, 60000, config.health_interval_ms)) error = "Invalid health_interval_ms";
            else if (key == "health_failure_threshold" && !parse_int(value, 1, 100, config.health_failure_threshold)) error = "Invalid health_failure_threshold";
            else if (key == "health_recovery_threshold" && !parse_int(value, 1, 100, config.health_recovery_threshold)) error = "Invalid health_recovery_threshold";
            else if (key != "listen" && key != "default_region" && key != "geoip_database" && key != "trust_proxy" &&
                     key != "development_allow_client_ip_override" && key != "router_token" && key != "timeout_ms" &&
                     key != "max_connections" && key != "worker_threads" && key != "health_interval_ms" &&
                     key != "health_failure_threshold" && key != "health_recovery_threshold") {
                error = "Unknown router key on line " + std::to_string(line_number) + ": " + key;
            }
            if (!error.empty()) return false;
        } else if (section.rfind("edges.", 0) == 0 && key == "url") {
            Endpoint endpoint;
            if (!parse_url(value, endpoint)) {
                error = "Invalid edge URL on line " + std::to_string(line_number);
                return false;
            }
            config.edges[section.substr(6)] = endpoint;
        } else {
            error = "Unknown config entry on line " + std::to_string(line_number);
            return false;
        }
    }

    config.mode = "router";
    if (config.edges.empty() || config.edges.count(config.default_region) == 0) {
        error = "Router needs edges and a valid default_region";
        return false;
    }
    return true;
}
