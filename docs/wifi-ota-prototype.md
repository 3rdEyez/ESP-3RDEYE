# Explicit BLE-controlled Wi-Fi OTA (local candidate)

The local 0.2.5 implementation now includes the product entry: authenticated BLE
open/close requests, real status/ACK reads, App instructions and a paused exit to
a fresh control session. It has not been deployed. Installed dual-slot firmware
remains 0.2.4. The built candidate is unsigned and must not be flashed as built.

## Protocol and product flow

Standard Control v1.2, capabilities and all existing UUIDs are unchanged. An
optional encrypted/authenticated read+write characteristic
`4d89f6a0-73b9-4f14-9d3e-63b2145a0007` negotiates the maintenance extension by
presence and status schema 1. Old firmware lacks it and remains controllable;
an unsigned seed exposes signing-not-ready without starting networking.

Request: exactly 10 bytes, schema 1 at byte 0, action open=1/close=2 at byte 1,
nonzero little-endian request ID at bytes 2..5, window ID at 6..9 (zero for open,
matching nonzero current ID for close). Recent eight requests are cached for
idempotent retries; retries do not restart or extend a window. Same request ID
with changed payload is rejected. This is a bounded retry cache, not permanent
history or an authorization credential.

Status: 18-byte header followed by printable ASCII SSID/password. Schema=1 at 0;
state Closed0/Opening1/Open2/Uploading3/Closing4/Committed5/Failed6 at 1;
result Ok0/Busy1/Unsupported2/Invalid3/NotReady4/StaleWindow5/Internal6 at 2;
reserved zero at 3; ACK request ID at 4..7; current/last window ID at 8..11;
remaining milliseconds at 12..15; SSID/password lengths at 16/17 (max 32/64).
A GATT write completion is never an open/close ACK. Poll the status and match the
request ID and terminal state. Closed retains its window ID for reconciliation.
Committed means image selected and reboot validation pending, not upgrade success.

Only the existing authenticated BLE peer requests opening/closing. Opening
invalidates the control lease/queue and waits for the sole motion task's
maintenance-epoch stop ACK. Once opened, any device joined to the temporary
WPA2 AP may upload from `http://192.168.4.1/`; no upload-side BLE pairing or
ongoing BLE connection is required. BLE loss does not cancel the AP/upload.
Window default is 120 seconds, an implementation setting rather than a permanent
user preference. Password/SSID are RAM-only and not logged or persisted.

App explicitly pauses/HALTs before entering, reads real device ACK/status and
shows manual Wi-Fi/browser instructions. Missing extension, signing-not-ready,
unknown/disconnected state and committed state have separate handling. Local
deadline expiry is not proof of closure. Closing/timeout clears networking and
creates no movement; explicit App exit establishes a fresh CLAIM with auto-start
false. The user must explicitly enable control. Reconnection reads the actual
firmware version and does not infer update success from upload or disconnect.

## Image and native authenticity policy

Only the inactive exact 0x170000-byte OTA slot is writable; running app must be
VALID. Exact length, received/stored SHA, real signed image board tag, chip,
project, version and official RSA signature validation precede selection. Close
and final commit are serialized; a close after commit returns Busy. Timeout,
cancellation or rejection never pretends that a committed image was undone.

Native IDF 5.5.4 RSA3072 signed-app verification without hardware Secure Boot is
used. No formal key is generated, no private-key path is embedded, and automatic
build signing, flash encryption, hardware Secure Boot and eFuse anti-rollback
are disabled. Trust comes from the first signature block of the running signed
app. First provision a signed USB seed; running unsigned 0.2.4 cannot establish
this OTA trust. Only one signer is supported; replacement/loss needs a new USB
signed seed. Software verification does not protect against physical Flash writes.

Offline `.sota` tool checks structure/length/SHA and refuses unsigned output, but
these are not cryptographic verification. Real device signature checking uses
`esp_ota_end`. The board tag offset 288 is pinned to IDF 5.5.4 and checked in the
actual build. Generic build-generated flash commands include more than an app
update and must not be used for this existing migrated device.

## Validation and deployment boundary

Firmware host tests cover control regressions, transfer lifecycle, real adapter
against native SDK mocks, window codecs/admission/retry and epoch stop-ACK
snapshots. App tests cover real status confirmation and maintenance exit. Mocks
are not hardware scheduling, cryptography, BLE long-read, Flash or radio tests.
See the task report and validation logs for exact counts and artifact hashes.

Four firmware profiles are separately built; only `ble_wifi_ota_prototype`
exposes the maintenance extension. Activate existing IDF 5.5.4, then run
`python tools/build_firmware.py ble_wifi_ota_prototype`. It explicitly reports
unsigned output. Both dual-slot profile defaults are version-controlled.

Remaining steps are authorized physical deployment: generate/independently back
up one signing key, sign and verify a seed, fresh double backups and app-only USB
write to the inactive slot, then real network/image-rejection testing. A signed
future app's explicit rollback API may reject the unsigned 0.2.4 fallback;
use a signed VALID seed for a separately approved no-output fault rehearsal.
Current bootloader PENDING-reset rollback and USB recovery are different paths.

Official native policy:
https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32c3/security/secure-boot-v2.html#signed-app-verification-without-hardware-secure-boot
