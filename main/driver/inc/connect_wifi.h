#ifndef CONNECT_WIFI_H
#define CONNECT_WIFI_H

void connect_wifi(void);

#endif // CONNECT_WIFI_H

#if CONFIG_SATORI_WIFI_OTA_PROTOTYPE
#include "esp_err.h"
#include <array>
// Maintenance-only reuse of STA lifecycle. Never reads/writes stored passwords,
// never starts legacy UDP. Caller owns total deadline and explicit credentials.
esp_err_t StartMaintenanceSta(const char* ssid,const char* password);
bool MaintenanceStaReady(std::array<unsigned char,4>& ip);
bool MaintenanceStaFailed();
esp_err_t StartSavedMaintenanceSta();
esp_err_t SaveMaintenanceNetwork(const char* ssid,const char* password);
bool HasSavedMaintenanceNetwork();
void StopMaintenanceSta();
#endif
