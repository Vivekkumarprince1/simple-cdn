#pragma once

#include "cdn_types.hpp"

#include <filesystem>
#include <string>

bool load_config(const std::filesystem::path& path, Config& config, std::string& error);
