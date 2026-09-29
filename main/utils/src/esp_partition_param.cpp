#include "esp_partition_param.h"
#include "config_value_parser.h"

namespace {
constexpr const char* PARTITION_NAME = "config";
}

EspPartitionParam &EspPartitionParam::GetInstance()
{
    static EspPartitionParam instance(PARTITION_NAME);
    return instance;
}

EspPartitionParam::EspPartitionParam(const std::string &partitionName) {
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, partitionName.c_str());
    if (partition == NULL) {
        // A missing optional config partition means the board uses its built-in
        // profile. Mapping/parse failures remain invalid and fail closed.
        ESP_LOGW("EspPartitionParam", "Optional config partition not found: %s", partitionName.c_str());
        return;
    }

    const size_t dataLen = partition->size;
    const void *map_ptr;
    esp_partition_mmap_handle_t map_handle;
    if (esp_partition_mmap(partition, 0, dataLen, ESP_PARTITION_MMAP_DATA, &map_ptr, &map_handle) != ESP_OK) {
        valid_ = false;
        ESP_LOGE("EspPartitionParam", "Cannot map board configuration");
        return;
    }
    std::string data(reinterpret_cast<const char *>(map_ptr), dataLen);
    const auto terminator = data.find_first_of(std::string("\0\xff", 2));
    if (terminator != std::string::npos) data.resize(terminator);
    std::istringstream iss(data);
    std::string line;
    const auto trim = [](std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    };
    while (std::getline(iss, line)) {
        line = trim(line);
        if (!IsWellFormedConfigLine(line)) { valid_ = false; continue; }
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        const auto key = trim(line.substr(0, pos));
        const auto value = line.substr(pos + 1);
        if (key.empty()) { valid_ = false; continue; }
        ESP_LOGI("EspPartitionParam", "loaded key: %s", key.c_str());
        params[key] = value;
    }

    esp_partition_munmap(map_handle);
}

int EspPartitionParam::GetIntParam(const std::string &key, int defaultValue) {
    auto it = params.find(key);
    if (it != params.end()) {
        int result{};
        if (ParseConfigInt(it->second, result)) return result;
        valid_ = false;
        ESP_LOGE("EspPartitionParam", "Invalid integer field: %s", key.c_str());
    }
    return defaultValue;
}

float EspPartitionParam::GetFloatParam(const std::string &key, float defaultValue)
{
    auto it = params.find(key);
    if (it!= params.end()) {
        float result{};
        if (ParseConfigFloat(it->second, result)) return result;
        valid_ = false;
        ESP_LOGE("EspPartitionParam", "Invalid numeric field: %s", key.c_str());
    }
    return defaultValue;
}

bool EspPartitionParam::GetBoolParam(const std::string &key, bool defaultValue)
{
    auto it = params.find(key);
    if (it!= params.end()) {
        if (it->second == "true") return true;
        if (it->second == "false") return false;
        valid_ = false;
        ESP_LOGE("EspPartitionParam", "Invalid boolean field: %s", key.c_str());
    }
    return defaultValue;
}


std::string EspPartitionParam::GetStringParam(const std::string &key, const std::string &defaultValue) {
    auto it = params.find(key);
    if (it != params.end()) {
        return it->second;
    }
    return defaultValue;
}
