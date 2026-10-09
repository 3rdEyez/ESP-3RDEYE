# Optional BLE diagnostics v1

Firmware 0.2.3 adds `4d89f6a0-73b9-4f14-9d3e-63b2145a0006`, an
encrypted, authenticated, bonded read-only characteristic in the existing
service. It has no write/notify/control operation and does not renew a lease.
The DeviceInfo format and all v1.2 control frames, opcodes and reserved bytes are unchanged.
Older Apps ignore the characteristic; Apps may attempt a bounded read on firmware
0.2.3 or later and continue using the existing control protocol if it is absent.

The 20-byte value uses little-endian integers:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 1 | Diagnostic schema, 1 |
| 1 | 1 | ESP-IDF `esp_reset_reason()` numeric value |
| 2 | 1 | Last recorded stop: none=0, HALT=1, RELEASE=2, link lost=3, lease expired=4, output fault=5, storage fault=6, work queue fault=7, host reset=8 |
| 3 | 1 | Latched faults: PWM=bit0, bond storage=bit1, work queue=bit2, identity storage=bit3, invalid startup configuration=bit4; bits5–7 zero |
| 4 | 4 | Uptime in seconds, modulo 2^32 |
| 8 | 4 | GAP disconnect count, saturating |
| 12 | 2 | Lease expiry count, saturating; RELEASE ACK-window expiry is excluded |
| 14 | 2 | Notification allocation/send failures, saturating |
| 16 | 2 | Last raw NimBLE GAP disconnect reason; 0 before any disconnect |
| 18 | 2 | Reserved, zero |

Reference vector: `0103040178563412090000000200050013020000` means reset
reason 3, lease expired, PWM fault, uptime 0x12345678, 9 disconnects, 2 lease
expiries, 5 notification failures, GAP reason 0x213. Both codecs test this vector.

Records are RAM only and reset on reboot. The record is copied under the existing
session critical section; uptime is sampled separately. This is an observation
of recent/history events, not a claim of physical pose or servo power state.
Loss of diagnostic reads must not affect motion scheduling, CLAIM/ARM or the
2-second heartbeat. App reads are bounded to 500 ms and at most once every
5 seconds; late reads from retired connections are ignored. The App retains the
last sample timestamp after read failures and labels stale data accordingly.

Faults that prevent BLE advertising or authenticated reads are visible only in
boot/error logs, not remotely through this characteristic. No new diagnostics
log or wire field contains passkeys, tokens, credentials or bond identities.
Boot logs include reset reason, project version and four bytes of the public
application ELF digest to distinguish builds. This is not a signature check.

Ending filming uses existing RELEASE: cancel local action/reconnect scheduling,
wait for the device's stop/release ACK, then disconnect. Without an ACK, report
local cancellation and unconfirmed device stop; existing disconnect/6-second
lease behavior remains the fallback. HALT, RELEASE, disconnect and lease expiry
do not cut servo power or switch off the entire device. No safe angle is assumed.
