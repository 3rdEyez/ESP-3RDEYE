#pragma once
#include "esp_ota_ops.h"
struct esp_image_sig_public_key_digests_t { unsigned num_digests; };
esp_err_t esp_secure_boot_get_signature_blocks_for_running_app(bool, esp_image_sig_public_key_digests_t*);
