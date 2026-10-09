#pragma once
#include <string_view>
namespace satori::ota {
// Same WPA2 personal-network limits as transient input. No logging/defaults.
inline bool SavedMaintenanceCredentialsValid(std::string_view ssid,std::string_view password) {
    if(ssid.empty()||ssid.size()>32||password.size()<8||password.size()>63)return false;
    for(const unsigned char c:ssid)if(c<0x20||c==0x7f)return false;
    for(const unsigned char c:password)if(c<0x20||c>0x7e)return false;
    return true;
}
}
