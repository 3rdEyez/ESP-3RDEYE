#pragma once
#include "esp_err.h"

// Initialize the shared NVS partition without erasing unrelated Wi-Fi or bond data.
esp_err_t InitializeSharedNvs();
