#pragma once
#include <cstdint>
namespace satori::ota {
constexpr std::uint32_t kMaintenanceIdleMs=600000;
constexpr std::uint32_t kMaintenanceUploadMs=120000;
inline std::uint32_t MaintenanceRemaining(std::uint32_t now,std::uint32_t start,bool uploading,std::uint32_t upload_start) {
    const auto elapsed=static_cast<std::uint32_t>(now-(uploading?upload_start:start));
    const auto limit=uploading?kMaintenanceUploadMs:kMaintenanceIdleMs;
    return elapsed<limit?limit-elapsed:0;
}
}
