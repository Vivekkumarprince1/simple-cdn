#pragma once

#include <string>

bool initialize_geoip(const std::string& database_path);
std::string geoip_region_for(const std::string& ip);
