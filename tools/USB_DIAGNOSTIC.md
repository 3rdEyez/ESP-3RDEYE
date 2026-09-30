# USB serial diagnostics

Use `tools/usb_diagnostic_capture.py` to correlate kernel USB events, operator marks, and read-only esptool output. Select the actual serial node on the host. Captures and flash reads may contain local paths, addresses, and device data; save them outside the repository and do not commit them.

The collector uses journald's `__REALTIME_TIMESTAMP` and `__MONOTONIC_TIMESTAMP` fields for kernel event time. An esptool connection may reset the chip or switch it to download mode, so a read is not passive with respect to USB enumeration. If the device disappears or Linux reports repeated descriptor errors, stop probing before attempting a write.

Use `--help` for capture, mark, probe, and backup options. A short successful read does not establish that a complete application or flash backup is available. Verify the full backup before flashing or rollback.
