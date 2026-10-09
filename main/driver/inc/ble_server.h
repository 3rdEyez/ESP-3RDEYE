#pragma once
#include "esp_err.h"
esp_err_t StartBlePrimary();
void StartBleMaintenanceConsole();

void SetBleBootControlAllowed(bool allowed);
bool BleStartupHealthy();
unsigned BleControlCycleCount();
// Maintenance uses a worker; normal control requires a fresh CLAIM afterwards.
bool BeginBleOtaMaintenance(unsigned short peer);
bool BleOtaMaintenanceStopped();
bool BleOtaPeerStillAuthorized(unsigned short peer);

void EndBleOtaMaintenance();
unsigned BleOtaConnectionEpoch();
