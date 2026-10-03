#pragma once
#include "esp_err.h"
esp_err_t StartBlePrimary();
void StartBleMaintenanceConsole();

void SetBleBootControlAllowed(bool allowed);
bool BleStartupHealthy();
unsigned BleControlCycleCount();
// Local prototype only: future authenticated BLE worker may explicitly enter.
bool BeginBleOtaMaintenance(unsigned short peer);
bool BleOtaMaintenanceStopped();
bool BleOtaPeerStillAuthorized(unsigned short peer);
