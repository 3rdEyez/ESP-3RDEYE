# SatoriEye BLE A0/A1 firmware setup

Protocol source of truth: [`SatoriEye_BLE_Protocol_v1.md`](SatoriEye_BLE_Protocol_v1.md), SHA-256 `13e6c166aa938bb8904dd58e30645c7dcce7678d977afa472c1075a84e1ec462`. Keep the Flutter mirror byte-identical. v1.0 golden vectors SHA-256: `9f6a9d4b9759c9b36a7ef9c7f6aa0412f15ec8a81cd96128faec62917edbb14e`; v1.1 management vectors SHA-256: `a8d2869740b370af3e003f086e0f076c1db16ef42ef95222f1bb543493d2ad8a`; v1.2 shared pairing vectors SHA-256: `0550f136fb109da8769a9a32ff3dfeb25f095e85419d9961e87a7c6a8982b9a7`.

## Build profiles

The mutually-exclusive `SATORI_TRANSPORT` Kconfig choice selects `ble_primary` or `legacy_udp`. Build directories and sdkconfig files must stay separate. BLE profile defaults to blank Wi-Fi credentials and does not start Wi-Fi.

With ESP-IDF 5.5.4 activated (the build helper rejects other versions):

```sh
tools/build_firmware.sh ble_primary
tools/build_firmware.sh legacy_udp
```

Neither build command flashes a board. The helper keeps each ignored sdkconfig under its profile build directory and never reads a developer's root sdkconfig, which may contain Wi-Fi credentials. BLE uses NimBLE LE Secure Connections, display-only six-digit passkey, up to eight saved phone bonds, one active central link, and encrypted/authenticated characteristics. Any saved phone may connect while the device is idle; a ninth phone is rejected and bonds are never automatically evicted.

## Initialize identity and pair

On healthy blank NVS with no residual bonds, first BLE startup creates a random device identity and initializes the pairing code to **123456**. The app shows this default explicitly. Any phone with the current code may pair while the device is idle, up to the eight-phone limit; an owner phone does not need to open a transfer window. Connect the ESP32-C3 USB Serial/JTAG port (commonly `/dev/ttyACM0`) and run `ble-pair-card` only if the saved code needs to be checked; never paste codes into logs, issues, or source control. `ble-provision` remains an explicit maintenance fallback for a blank device. Identity, bonds, and changed pairing code survive firmware upgrades.

Pair from an Android phone using the six-digit code. The firmware requires authenticated, bonded Secure Connections and rejects unauthenticated links. Advertising stops while connected and restarts after disconnect, making the device available to another saved phone. `OPEN_TRANSFER` and `CANCEL_TRANSFER` are unsupported in firmware 0.2.2. Any securely paired phone may change the shared code; the code is persisted before success ACK. `ble-recover-binding` clears stored bonds and returns the pairing code to `123456` while preserving device identity and board config. To pair a ninth phone, use this USB recovery flow after backing up persistent data.

## Startup profile and board configuration

Pairing, reconnecting, subscribing, and `CLAIM` do not energize servos. `ARM` enables output only after the PWM driver initializes successfully. The hidden built-in `satori_c3_v1` profile uses logical startup values `[1500, 1500, 1500]`, matching the legacy Qt app's initial controls. These are logical inputs, not measured physical positions or a claim of mechanical safety. Existing GPIO, calibration, angle limits, and CH3/CH2 coupling still determine the resulting hardware output.

The separate config partition can override the built-in logical target by setting `BLE_STARTUP_CH1`, `BLE_STARTUP_CH2`, and `BLE_STARTUP_CH3` to values in the protocol's 500–2500 range and setting `BLE_STARTUP_CONFIRMED=true`. A false or absent confirmation uses the built-in profile. Partial, malformed, or out-of-range explicit values fail closed. Preserve board GPIO, scale, offset, zero, angle limits, reversal, and coupling values. Invalid pins or calibration keep ARM unavailable. Use the established USB config maintenance path for board-specific overrides.

## Recovery and rollback

Before changing persistent data, activate ESP-IDF 5.5.4, then run `tools/backup_device.sh PORT PRIVATE_OUTPUT_DIRECTORY`. It reads the raw default NVS and separate board-config partitions and creates mode-0600 files outside the repository; NVS may contain Wi-Fi credentials and BLE pairing secrets, so keep them private. `ble-recover-binding` preserves identity and board configuration while resetting the pairing code and clearing BLE bonds. If default NVS itself cannot initialize, inspect and back it up first. The explicit USB command `ble-repair-nvs --erase-entire-default-nvs-after-backup` erases the **entire default NVS partition**, including Wi-Fi credentials, bonds, and identity; BLE identity must then be provisioned again. The separate `config` partition at `0x300000` is not erased by that command. There is no automatic NVS erase on boot.

Rollback to `legacy_udp` requires a separately built legacy profile. The generated ESP-IDF flash arguments contain only the bootloader (`0x0`), partition table (`0x8000`), and app image (`0x10000`); they do not write default NVS (`0x9000`) or the board config partition (`0x300000`). Review `build/<profile>/flash_args` before any later hardware flash. Profile changes preserve NVS, device identity, Wi-Fi credentials, bonds, and board calibration. Legacy UDP should only be enabled on a controlled network.

## Validation status

Host codec/session/motion tests consume the shared JSON corpus. Host legacy parser and config-value tests are independent. Target builds and actual hardware security, MTU 23, output range, lock-screen/background, and reconnection behavior must be recorded separately. No hardware flashing or motion test is implied by a successful software build.

Hardware update (2026-09-29): BLE 0.2.2 has been flashed and tested with the satori Linux Bluetooth adapter. Pairing, authenticated GATT, session acknowledgements, bonded reconnect, and lease expiry passed without enabling outputs. See [hardware results](HARDWARE_TEST_2026-09-29.md); mobile and motion validation remain pending.
