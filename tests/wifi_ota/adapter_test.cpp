#include "ota_idf_sink.hpp"
#include "ota_board_tag.hpp"
#include "esp_app_desc.h"
#include "esp_secure_boot.h"
#include <cassert>
#include <cstring>
#include <vector>

// This integration compiles the actual IDF adapter against in-memory SDK mocks.
// Official signature decisions are injected. The toy checksum is NOT a crypto test.
using namespace satori::ota;
namespace {
esp_partition_t run_part, next_part;
esp_ota_img_states_t state;
esp_app_desc_t current_desc, offered_desc;
unsigned begins, writes, ends, aborts, selections, policy_checks;
bool live, policy_ok, read_ok;
int begin_rc, write_rc, end_rc, selection_rc, state_rc, description_rc;
int sha_start_rc, sha_update_rc, sha_finish_rc;
unsigned key_count;
bool guard_allowed;
unsigned guard_calls, guard_fail_at, partition_reads;
std::vector<unsigned char> flash;
[[maybe_unused]] bool ContinueGuard() {
    ++guard_calls;
    return guard_allowed && (guard_fail_at == 0 || guard_calls < guard_fail_at);
}
void Reset() {
    run_part = {ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x10000, kSlotSize, false};
    next_part = {ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x180000, kSlotSize, false};
    state = ESP_OTA_IMG_VALID;
    current_desc = {}; offered_desc = {};
    std::strcpy(current_desc.version, "0.2.4"); std::strcpy(offered_desc.version, "0.2.5");
    std::strcpy(current_desc.project_name, "satori"); std::strcpy(offered_desc.project_name, "satori");
    begins = writes = ends = aborts = selections = policy_checks = 0;
    live = false; policy_ok = read_ok = true;
    begin_rc = write_rc = end_rc = selection_rc = state_rc = description_rc = 0;
    sha_start_rc = sha_update_rc = sha_finish_rc = 0; key_count = 1;
    guard_allowed = true; guard_calls = guard_fail_at = partition_reads = 0;
    flash.clear();
}
std::array<unsigned char,32> ToyHash(const unsigned char* bytes, std::size_t count) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < count; ++i) sum += bytes[i];
    std::array<unsigned char,32> out{};
    for (unsigned i = 0; i < 32; ++i) out[i] = static_cast<unsigned char>(sum + i);
    return out;
}
std::vector<unsigned char> Image() {
    std::vector<unsigned char> image(1024, 1);
    std::memcpy(image.data() + kBoardTagImageOffset, &kExpectedBoardTag, sizeof(BoardTag));
    return image;
}
ImageManifest ImageMetadata(const std::vector<unsigned char>& image) {
    return {static_cast<std::uint32_t>(image.size()), ToyHash(image.data(), image.size()), "0.2.5"};
}
Manifest Core(const ImageManifest& manifest) {
    return {"satori_c3_v1", "esp32c3", Slot::Ota1, manifest.image_size, manifest.sha256};
}
}
const esp_partition_t* esp_ota_get_running_partition() { return &run_part; }
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) { return &next_part; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* out) { *out = state; return state_rc; }
esp_err_t esp_ota_begin(const esp_partition_t* partition, std::size_t size, esp_ota_handle_t* handle) {
    ++begins; assert(partition == &next_part && !live);
    if (begin_rc) return begin_rc;
    flash.assign(size, 0xff); *handle = 7; live = true; return 0;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* data, std::size_t size) {
    assert(live && handle == 7); ++writes;
    if (write_rc) return write_rc;
    static std::size_t position = 0;
    if (writes == 1) position = 0;
    assert(position + size <= flash.size());
    std::memcpy(flash.data() + position, data, size); position += size; return 0;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle) { assert(live && handle == 7); ++ends; live = false; return end_rc; }
esp_err_t esp_ota_abort(esp_ota_handle_t handle) { assert(live && handle == 7); ++aborts; live = false; return 0; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* p) { assert(p == &next_part && !live); ++selections; return selection_rc; }
esp_err_t esp_ota_get_partition_description(const esp_partition_t*, esp_app_desc_t* out) { *out = offered_desc; return description_rc; }
esp_err_t esp_partition_read(const esp_partition_t*, std::size_t offset, void* out, std::size_t size) {
    ++partition_reads;
    if (!read_ok || offset > flash.size() || size > flash.size() - offset) return -1;
    std::memcpy(out, flash.data() + offset, size); return 0;
}
const esp_app_desc_t* esp_app_get_description() { return &current_desc; }
esp_err_t esp_secure_boot_get_signature_blocks_for_running_app(bool verify, esp_image_sig_public_key_digests_t* keys) {
    assert(verify); ++policy_checks; keys->num_digests = key_count; return policy_ok ? 0 : -1;
}
void mbedtls_sha256_init(mbedtls_sha256_context* ctx) { ctx->sum = 0; }
void mbedtls_sha256_free(mbedtls_sha256_context*) {}
int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int) { ctx->sum = 0; return sha_start_rc; }
int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const unsigned char* bytes, std::size_t size) {
    if (sha_update_rc) return sha_update_rc;
    for (std::size_t i = 0; i < size; ++i) ctx->sum += bytes[i];
    return 0;
}
int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char* out) {
    if (sha_finish_rc) return sha_finish_rc;
    for (unsigned i = 0; i < 32; ++i) out[i] = static_cast<unsigned char>(ctx->sum + i);
    return 0;
}
int mbedtls_sha256(const unsigned char* data, std::size_t size, unsigned char* out, int) {
    const auto hash = ToyHash(data, size); std::memcpy(out, hash.data(), hash.size()); return 0;
}

int main() {
#if !TEST_NATIVE_POLICY

    {
        Reset(); const auto image = Image(); const auto signed_manifest = ImageMetadata(image);
        IdfOtaSink sink(signed_manifest);
        assert(!sink.Begin(Core(signed_manifest)));
        assert(begins == 0 && policy_checks == 0); sink.Abort(); assert(aborts == 0);
    }
    return 0;
#else
    Reset(); assert(OfficialSignaturePolicyReady());
    for (unsigned failure = 0; failure < 21; ++failure) {
        Reset(); const auto image = Image(); auto signed_manifest = ImageMetadata(image); auto core = Core(signed_manifest);
        
        switch (failure) {
        case 0: policy_ok = false; break;
        case 1: key_count = 0; break;
        case 2: state = ESP_OTA_IMG_PENDING_VERIFY; break;
        case 3: state_rc = -1; break;
        case 4: next_part.address = run_part.address; break;
        case 5: next_part.size = kSlotSize - 1; break;
        case 6: next_part.encrypted = true; break;
        case 7: core.target_slot = Slot::Ota0; break;
        case 8: core.board = "satori_s3_v1"; break;
        case 9: core.chip = "esp32s3"; break;
        case 10: ++core.image_size; break;
        case 11: ++core.sha256[0]; break;
        case 12: signed_manifest.version = "0.2.4"; break;
        case 13: signed_manifest.version = "0.2.3"; break;
        case 14: sha_start_rc = -1; break;
        case 15: key_count = 2; break;
        case 16: signed_manifest.image_size = 0; core = Core(signed_manifest); break;
        case 17: signed_manifest.image_size = kSlotSize + 1; core = Core(signed_manifest); break;
        case 18: next_part.subtype = ESP_PARTITION_SUBTYPE_OTHER; break;
        case 19: run_part.address = 0x20000; break;
        case 20: std::memset(current_desc.version, 'x', sizeof(current_desc.version)); break;
        }
        IdfOtaSink sink(signed_manifest);
        assert(!sink.Begin(core) && begins == 0 && selections == 0);
        assert(!sink.SetBootTarget(Slot::Ota1)); sink.Abort(); assert(aborts == 0);
    }
    for (unsigned failure = 0; failure < 12; ++failure) {
        Reset(); auto image = Image();
        if (failure == 5) image[kBoardTagImageOffset] ^= 1; // valid digest of wrong board tag
        const auto signed_manifest = ImageMetadata(image); 
        IdfOtaSink sink(signed_manifest); assert(sink.Begin(Core(signed_manifest)));
        if (failure == 0) image[0] ^= 1;
        assert(sink.Write(image.data(), image.size()));
        if (failure == 1) policy_ok = false;
        if (failure == 2) end_rc = -1;
        if (failure == 3) sha_finish_rc = -1;
        if (failure == 4) read_ok = false;
        if (failure == 6) std::strcpy(offered_desc.version, "0.2.6");
        if (failure == 7) std::strcpy(offered_desc.project_name, "other");
        if (failure == 8) offered_desc.secure_version = 1;
        if (failure == 9) description_rc = -1;
        if (failure == 10) ++flash[0]; // only stored Flash corrupt; received SHA matches
        if (failure == 11) std::memset(offered_desc.version, 'x', sizeof(offered_desc.version));
        assert(sink.FinishVerifyAuthenticity() != Verification::Verified);
        assert(!sink.SetBootTarget(Slot::Ota1) && selections == 0);
        sink.Abort(); sink.Abort(); assert(!live && aborts <= 1);
    }
    {
        Reset(); const auto image = Image(); const auto manifest = ImageMetadata(image);
        IdfOtaSink sink(manifest); assert(sink.Begin(Core(manifest)));
        assert(!sink.Write(image.data(), image.size()+1));
        assert(sink.FinishVerifyAuthenticity() == Verification::IntegrityFailure);
        sink.Abort(); sink.Abort(); assert(aborts == 1 && writes == 0 && selections == 0);
    }
    {
        Reset(); const auto image = Image(); const auto manifest = ImageMetadata(image); 
        { IdfOtaSink sink(manifest); assert(sink.Begin(Core(manifest))); write_rc = -1;
          assert(!sink.Write(image.data(), image.size())); }
        assert(aborts == 1 && !live && selections == 0);
    }
    {
        Reset(); const auto image = Image(); const auto manifest = ImageMetadata(image); 
        IdfOtaSink sink(manifest); assert(sink.Begin(Core(manifest)));
        assert(sink.Write(image.data(), 100) && sink.Write(image.data()+100, image.size()-100));
        assert(sink.FinishVerifyAuthenticity() == Verification::Verified && ends == 1 && !live);
        assert(!sink.SetBootTarget(Slot::Ota0));
        assert(sink.SetBootTarget(Slot::Ota1)); assert(!sink.SetBootTarget(Slot::Ota1));
        sink.Abort(); assert(selections == 1 && aborts == 0);
    }
    {
        Reset(); const auto image = Image(); const auto manifest = ImageMetadata(image); 
        IdfOtaSink sink(manifest); assert(sink.Begin(Core(manifest)));
        assert(sink.Write(image.data(), image.size()) && sink.FinishVerifyAuthenticity() == Verification::Verified);
        state = ESP_OTA_IMG_PENDING_VERIFY;
        assert(!sink.SetBootTarget(Slot::Ota1) && selections == 0);
    }
    {
        Reset(); const auto image = Image(); const auto manifest = ImageMetadata(image);
        IdfOtaSink sink(manifest); assert(sink.Begin(Core(manifest)));
        sink.Abort(); sink.Abort();
        assert(!sink.Begin(Core(manifest)) && !sink.SetBootTarget(Slot::Ota1));
        assert(begins == 1 && aborts == 1 && selections == 0);
    }
    {
        Reset(); const auto image = Image(); const auto manifest = ImageMetadata(image);
        IdfOtaSink sink(manifest); assert(sink.Begin(Core(manifest)));
        assert(sink.Write(image.data(), image.size()));
        assert(sink.FinishVerifyAuthenticity() == Verification::Verified);
        sink.Abort();
        assert(!sink.Begin(Core(manifest)) && !sink.SetBootTarget(Slot::Ota1));
        assert(begins == 1 && ends == 1 && aborts == 0 && selections == 0);
    }
    {
        Reset(); const auto image = Image(); const auto manifest = ImageMetadata(image);
        IdfOtaSink sink(manifest); begin_rc = -1;
        assert(!sink.Begin(Core(manifest))); sink.Abort();
        assert(!sink.Begin(Core(manifest)) && begins == 1 && aborts == 0 && selections == 0);
    }
    {
        Reset(); const auto image = Image(); const auto manifest = ImageMetadata(image);
        IdfOtaSink sink(manifest); assert(sink.Begin(Core(manifest))); sha_update_rc = -1;
        assert(!sink.Write(image.data(), image.size())); sink.Abort();
        assert(writes == 1 && aborts == 1 && selections == 0);
    }
    {
        Reset(); const auto image = Image(); const auto manifest = ImageMetadata(image);
        IdfOtaSink sink(manifest); assert(sink.Begin(Core(manifest)));
        assert(sink.Write(image.data(), image.size()) && sink.FinishVerifyAuthenticity() == Verification::Verified);
        selection_rc = -1; assert(!sink.SetBootTarget(Slot::Ota1)); sink.Abort();
        assert(!sink.SetBootTarget(Slot::Ota1) && selections == 1 && aborts == 0);
    }
    {
        Reset(); const auto image = Image(); const auto manifest = ImageMetadata(image);
        guard_allowed = false;
        IdfOtaSink sink(manifest, ContinueGuard);
        assert(!sink.Begin(Core(manifest)) && begins == 0 && policy_checks == 0);
        sink.Abort(); assert(aborts == 0 && selections == 0);
    }
    {
        Reset(); const auto image = Image(); const auto manifest = ImageMetadata(image);
        IdfOtaSink sink(manifest, ContinueGuard); assert(sink.Begin(Core(manifest)));
        guard_allowed = false;
        assert(!sink.Write(image.data(), image.size()) && writes == 0);
        sink.Abort(); sink.Abort(); assert(aborts == 1 && selections == 0);
    }
    {
        Reset(); const auto image = Image(); const auto manifest = ImageMetadata(image);
        IdfOtaSink sink(manifest, ContinueGuard); assert(sink.Begin(Core(manifest)));
        assert(sink.Write(image.data(), image.size())); guard_allowed = false;
        assert(sink.FinishVerifyAuthenticity() != Verification::Verified);
        assert(ends == 0 && partition_reads == 0);
        sink.Abort(); assert(aborts == 1 && selections == 0);
    }
    {
        Reset(); const auto image = Image(); const auto manifest = ImageMetadata(image);
        IdfOtaSink sink(manifest, ContinueGuard); assert(sink.Begin(Core(manifest)));
        assert(sink.Write(image.data(), image.size()));
        assert(sink.FinishVerifyAuthenticity() == Verification::Verified);
        guard_allowed = false;
        assert(!sink.SetBootTarget(Slot::Ota1) && selections == 0);
        sink.Abort(); assert(aborts == 0);
    }
    {
        Reset(); auto image = Image(); image.resize(8192, 1);
        const auto manifest = ImageMetadata(image);
        IdfOtaSink sink(manifest, ContinueGuard); assert(sink.Begin(Core(manifest)));
        assert(sink.Write(image.data(), image.size()));
        // Begin, Write, Finish entry, first read chunk pass; second chunk cancels.
        guard_fail_at = 5;
        assert(sink.FinishVerifyAuthenticity() != Verification::Verified);
        assert(guard_calls == 5 && partition_reads == 1 && ends == 0);
        assert(!sink.SetBootTarget(Slot::Ota1)); sink.Abort(); sink.Abort();
        assert(aborts == 1 && selections == 0);
    }

#endif
}
