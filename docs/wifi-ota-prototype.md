# Local Wi-Fi OTA prototype (ESP-IDF 5.5.4, ESP32-C3)

This branch is an offline implementation candidate, not deployed firmware. The
installed device remains the separately verified dual-slot 0.2.4 migration.
The 0.2.5 candidate is unsigned and must not be flashed as built.

## Implemented

- Pure C++ transfer state machine and an adapter using official `esp_ota_*`
  APIs. Only the inactive, exact 0x170000-byte OTA slot can be written. Running
  firmware must already be VALID. Exact length, streaming and stored-Flash SHA,
  actual image board tag, chip, project, version and native RSA signature checks
  precede boot-target selection. Cancellation/timeout guards prevent commit.
- Native signed-app verification without hardware Secure Boot. The isolated
  profile enables RSA3072 verification and disables automatic signing, flash
  encryption and eFuse anti-rollback. Trust comes from the first signature block
  of the running signed app. No key, root, eFuse or private-key path is installed.
- Explicit maintenance API: a paired BLE peer requests stopping the control
  task and discarding the session/queue. A temporary RAM-only WPA2 SoftAP and
  browser upload page accept a signed `.sota` package from any joined device.
  The uploader does not need BLE pairing or an ongoing BLE connection.
- Window default: 120 seconds, implementation setting rather than a permanent
  user preference. A successful explicit install selects the image and restarts;
  timeout, cancellation or error closes networking and does not restore motion.
- Offline package inspection/container generation and rollback state simulation.
  SHA and signature-block shape do not establish authenticity. Unsigned package
  output is rejected. Production signature verification remains the SDK's job.

## Deliberately incomplete integration

No existing GATT/USB/startup caller invokes the maintenance API. There is no App
button, credentials/status bridge or exit-to-fresh-control-session flow yet.
The link anchor retains the complete service for realistic build size; it never
executes it. App additions are package parsing/preflight and design notes only.
Thus this branch cannot currently open an upgrade window through the product.
The first signed seed and real Wi-Fi/heap/coexistence validation remain necessary.

Build: activate the existing IDF 5.5.4 toolchain, then
`python tools/build_firmware.py ble_wifi_ota_prototype`. The result explicitly
says “App built but not signed”. Never use its generated generic flash command:
it includes bootloader, partition table and initial otadata, beyond an app update.
Both dual-slot profile defaults are tracked here; the earlier migration commit
omitted its ignored defaults file, which this commit repairs for reproducibility.

## Offline validation

Nine firmware host-test scripts and twelve Python tests pass. The actual sink is
compiled against SDK mocks with native policy disabled/enabled; cases cover wrong
slot, unsigned policy, corrupted Flash, signature rejection, wrong board/version,
truncation and cancellation before/within verification and before selection.
Mocks do not prove cryptographic verification, real Flash behavior or radio heap.
Browser script passes Node syntax checking. The App has 122 passing tests,
including eight new package/preflight tests; scoped Dart analysis is clean.

The linked unsigned image is 1,441,792 bytes. A native 4 KiB signature sector
leaves 61,440 bytes in its OTA slot. The signed board tag is at image offset 288,
verified against the actual output; this assumption is pinned to IDF 5.5.4.

## Signed seed and rollback boundary

The running unsigned 0.2.4 cannot supply the signer trust required by the OTA
policy. First provision one signed app by an explicitly approved USB app-only
write to currently inactive ota_0, preserving running ota_1 and all data/table/
bootloader regions. Verify the signed artifact offline with the public key and
perform complete readback before switching boot selection.

Only one signing key is supported by this prototype. Losing/replacing it needs
an approved new USB signed seed. Software signature checking does not defend
against physical Flash replacement. No home-grown PKI is used.

Do not assume the signed future app can call the SDK's explicit rollback API
against unsigned 0.2.4: the API validates the fallback image under its current
signature policy. Existing unsigned bootloader PENDING reset rollback and USB
recovery are separate paths. A real fault rehearsal should use a signed VALID
seed as fallback and a same-key signed fault candidate that never confirms boot
or enables outputs. Its local plan is not an executed test.

## Next implementation and device acceptance

1. Wire one explicit BLE window request and encrypted credential/status response,
   then a minimal App action opening manual Wi-Fi/browser instructions. Retain
   single-phone normal control, no automatic retry and fresh-session-only exit.
2. Approve key generation, storage/independent backup, signed USB seed and exact
   app/otadata writes. Reinspect current slot and make fresh double backups.
3. Approve a real temporary AP test: same-key valid update succeeds; unsigned,
   wrong-key, wrong-board, downgrade, truncation and timeout never select boot.
   Check heap and BLE/Wi-Fi coexistence on hardware.
4. Separately approve a no-output signed fault boot/reset/rollback rehearsal.
   Do not combine first signed provisioning with intentional startup failure.

Official policy reference:
https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32c3/security/secure-boot-v2.html#signed-app-verification-without-hardware-secure-boot
