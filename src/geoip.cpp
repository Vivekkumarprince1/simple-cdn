#include "geoip.hpp"

#include <unordered_map>

#ifdef CDN_WITH_MAXMINDDB
#include <maxminddb.h>

namespace {
MMDB_s database{};
bool database_open = false;

struct DatabaseCloser {
    ~DatabaseCloser() {
        if (database_open) MMDB_close(&database);
    }
} database_closer;
}  // namespace

bool initialize_geoip(const std::string& path) {
    if (path.empty()) return false;
    database_open = MMDB_open(path.c_str(), MMDB_MODE_MMAP, &database) == MMDB_SUCCESS;
    return database_open;
}

std::string geoip_region_for(const std::string& ip) {
    if (!database_open) return "unknown";
    int gai_error = 0;
    int mmdb_error = 0;
    const auto result = MMDB_lookup_string(&database, ip.c_str(), &gai_error, &mmdb_error);
    std::string code;
    if (gai_error == 0 && mmdb_error == MMDB_SUCCESS && result.found_entry) {
        MMDB_entry_data_s data{};
        if (MMDB_get_value(&result.entry, &data, "subdivisions", "0", "iso_code", nullptr) == MMDB_SUCCESS &&
            data.has_data && data.type == MMDB_DATA_TYPE_UTF8_STRING) {
            code.assign(data.utf8_string, data.data_size);
        }
    }

    static const std::unordered_map<std::string, std::string> regions = {
        {"CH","north"},{"DL","north"},{"HR","north"},{"HP","north"},{"JK","north"},{"LA","north"},{"PB","north"},{"RJ","north"},{"UP","north"},{"UK","north"},
        {"CT","west-central"},{"DN","west-central"},{"GA","west-central"},{"GJ","west-central"},{"MP","west-central"},{"MH","west-central"},
        {"AP","south-east"},{"AR","south-east"},{"AS","south-east"},{"BR","south-east"},{"JH","south-east"},{"KA","south-east"},{"KL","south-east"},{"MN","south-east"},{"ML","south-east"},{"MZ","south-east"},{"NL","south-east"},{"OD","south-east"},{"PY","south-east"},{"SK","south-east"},{"TN","south-east"},{"TG","south-east"},{"TR","south-east"},{"WB","south-east"},{"AN","south-east"},{"LD","south-east"}
    };
    const auto found = regions.find(code);
    return found == regions.end() ? "unknown" : found->second;
}
#else
bool initialize_geoip(const std::string&) { return false; }
std::string geoip_region_for(const std::string&) { return "unknown"; }
#endif
