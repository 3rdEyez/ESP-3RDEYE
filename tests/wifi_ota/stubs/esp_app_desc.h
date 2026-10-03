#pragma once
#include <cstdint>
struct esp_app_desc_t { std::uint32_t secure_version; char version[32], project_name[32]; };
const esp_app_desc_t* esp_app_get_description();
