# Firmware build, backup, flash, and rollback

Use ESP-IDF 5.5.4 and select one profile with `tools/build_firmware.sh ble_primary` or `tools/build_firmware.sh legacy_udp`. Review the generated partition table and flash arguments before writing. The BLE profile does not start Wi-Fi.

Before flashing, back up the complete device flash and the separate NVS and board configuration partitions to a private directory outside the repository. These regions may contain network credentials, pairing data, and mechanical calibration. Keep the backups private and verify their sizes and hashes. Confirm that the board has a stable USB serial connection and can enter the ROM download mode.

The application image starts at `0x10000`. An application-only update must preserve the bootloader, partition table, NVS, and board configuration. After writing, independently read back the application area and compare it with the intended image, then boot and check the firmware version. Do not use an entire-chip erase to troubleshoot an application update.

To roll back, write the previously backed-up application partition at `0x10000` and compare an independent readback before rebooting. A `--before no_reset` esptool option is appropriate only when the chip is already in download mode. If USB enumeration is unstable, stop and diagnose the connection before any write.
