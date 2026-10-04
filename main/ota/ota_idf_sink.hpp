#pragma once
#include "sdkconfig.h"
#if CONFIG_SATORI_WIFI_OTA_PROTOTYPE
#include "ota_manifest.hpp"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"

namespace satori::ota {
// Native IDF policy, never a matching hash or client-supplied root. No policy
// is enabled by the local prototype; initial signed USB baseline needs approval.
bool OfficialSignaturePolicyReady();
bool OfficialUpdateLayoutReady();
class IdfOtaSink final : public Sink {
public:
    explicit IdfOtaSink(const ImageManifest& manifest,bool (*continue_guard)()=nullptr);
    ~IdfOtaSink() override;
    bool Begin(const Manifest&) override;
    bool Write(const std::uint8_t*,std::size_t) override;
    void Abort() override;
    Verification FinishVerifyAuthenticity() override;
    bool SetBootTarget(Slot) override;
    Slot running_slot() const;
    Slot update_slot() const;
    bool running_valid() const;
private:
    ImageManifest manifest_;
    bool (*continue_guard_)(){nullptr};
    const esp_partition_t* running_{nullptr};
    const esp_partition_t* update_{nullptr};
    esp_ota_handle_t handle_{0};
    mbedtls_sha256_context hash_{};
    std::uint32_t written_{0};
    bool active_{false},verified_{false},selected_{false},attempted_{false};
};
} // namespace satori::ota
#endif
