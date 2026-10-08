#pragma once
#include <cstdint>

namespace satori::ble {
// GATT v2 is registered first and is independent of application service size.
// These handles describe the pinned SDK GATT service with Database Hash enabled.
struct StableGattLayout {
    std::uint16_t service{0};
    std::uint16_t changed{0};
    std::uint16_t hash{0};
    bool valid() const { return service == 1 && changed == 3 && hash == 10; }
};

template<class Gatt, class Gap, class Application>
int RegisterStableGatt(Gatt gatt, Gap gap, Application application) {
    gatt();  // Service Changed cannot move when GAP or application grows.
    gap();
    return application();
}

// Called only after the host has registered all attributes. Callback failures
// prevent advertising; no reset, pairing or credential operation belongs here.
template<class Hash, class MarkChanged, class StorageHealthy>
bool PrepareGattBoot(const StableGattLayout& layout, Hash hash,
                     MarkChanged mark_changed, StorageHealthy storage_healthy) {
    if (!layout.valid() || !hash()) return false;
    mark_changed(1, 0xffff);
    return storage_healthy();
}
} // namespace satori::ble
