#ifndef ESP_PARTITION_PARAM_H
#define ESP_PARTITION_PARAM_H

#include <string>
#include <map>
#include <sstream>
#include <vector>
#include <iostream>
#include "esp_partition.h"
#include "esp_log.h"

class EspPartitionParam {
public:
    bool IsValid() const { return valid_; }
    bool HasParam(const std::string &key) const { return params.find(key) != params.end(); }
    static EspPartitionParam &GetInstance(); // Singleton
    int GetIntParam(const std::string &key, int defaultValue);
    float GetFloatParam(const std::string &key, float defaultValue);
    bool GetBoolParam(const std::string &key, bool defaultValue);
    std::string GetStringParam(const std::string &key, const std::string &defaultValue);

private:
    EspPartitionParam(const std::string &partitionName);
    std::map<std::string, std::string> params;
    bool valid_ = true;
};

#endif // ESP_PARTITION_PARAM_H
