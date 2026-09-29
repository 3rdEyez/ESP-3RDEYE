#include "esp_partition_param.h"

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
        ESP_LOGE("EspPartitionParam", "Partition not found: %s", partitionName.c_str());
        return;
    }

    const size_t dataLen = partition->size;
    const void *map_ptr;
    esp_partition_mmap_handle_t map_handle;
    ESP_ERROR_CHECK(esp_partition_mmap(partition, 0, dataLen, ESP_PARTITION_MMAP_DATA, &map_ptr, &map_handle));
    std::string data(reinterpret_cast<const char *>(map_ptr), dataLen);
    std::istringstream iss(data);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '[') {
            continue;
        }
        size_t pos = line.find('=');
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            ESP_LOGI("EspPartitionParam", "loaded key: %s", key.c_str());
            params[key] = value;
        }
    }

    esp_partition_munmap(map_handle);
}

int EspPartitionParam::GetIntParam(const std::string &key, int defaultValue) {
    auto it = params.find(key);
    if (it != params.end()) {
        return std::stoi(it->second);
    }
    return defaultValue;
}

float EspPartitionParam::GetFloatParam(const std::string &key, float defaultValue)
{
    auto it = params.find(key);
    if (it!= params.end()) {
        return std::stof(it->second);
    }
    return defaultValue;
}

bool EspPartitionParam::GetBoolParam(const std::string &key, bool defaultValue)
{
    auto it = params.find(key);
    if (it!= params.end()) {
        return it->second == "true";
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
