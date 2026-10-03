# Computer-first LAN OTA (local 0.2.6 candidate)

Normal control remains BLE. The new encrypted/authenticated characteristic
`4d89f6a0-73b9-4f14-9d3e-63b2145a0008` lets an explicitly authenticated user
supply the target WPA2/WPA3-personal network in the App and open one maintenance
window. This is runtime configuration, not the old `config.ini` flasher.
Only RAM is used: no Wi-Fi password is read from the host or old config,
no credentials are migrated or saved to NVS, and legacy UDP control never starts.
The App labels this as **only this maintenance window**. Credentials must be
entered in the App, never sent in chat. Persistent provisioning and network scans
are outside this minimal implementation.

Motion is stopped and confirmed before networking. STA reuses the existing
`connect_wifi.cpp` ESP-IDF initialization/event model through a separate owned
maintenance lifecycle: RAM driver config, real GOT_IP, at most two reconnects
before initial readiness, no reconnect after an established link is lost,
and complete handler/netif/driver cleanup. Connection is bounded to 20 seconds;
the overall 120-second window includes that time. Closing while Opening is queued
behind this bounded connection step, so the client waits up to 26 seconds and
keeps control blocked until actual Closed. BLE reconnect/exit never restores
old motion or automatic ARM. New control requires a fresh CLAIM and explicit ARM.

## Wire contract

The normal control v1.2/DeviceInfo capabilities remain unchanged. Optional 0007
is the unchanged explicit AP fallback; 0008 is separately discoverable.

0008 write: schema=1 at byte 0; action 1=configure-and-open-LAN or 2=close at 1;
request_id LE32 at 2 (nonzero), window_id LE32 at 6 (zero for open, current
nonzero for close); SSID byte length at 10, password byte length at 11; bytes at
12. SSID is 1..32 bytes without NUL/control bytes; password is printable ASCII
8..63 bytes. Open/WEP and 64-hex PSKs are deliberately rejected. Close is exactly
12 bytes with zero lengths. App sends UTF-8 SSID with no normalization; Android
requests/checks a suitable MTU before sending one complete GATT write.

0008 read: schema1/state0..6/result0..6/detail at bytes0..3; request_id/window_id/
remaining_ms LE32 at4/8/12; current IPv4 network-order bytes16..19; token length
at20; reserved zero21..23; token bytes at24. Detail0=none,1=invalid config,
2=connection timeout,3=Wi-Fi failure,4=lost established link. Only Open/Uploading
return a 32-character random hex window token and IP. No SSID/password is ever
returned. Open means actual IP + registered HTTP listener, not GATT write ACK.
Requests are correlated and cached; a reused ID with different credentials is
Invalid (RAM-only SHA256 fingerprints), and duplicates do not extend the window.
AP and LAN share one active maintenance gate and cannot close each other.

## Upload from the computer

1. Build the normal OTA profile and sign its app with the already established
   signing process. This local candidate remains **unsigned and uninstalled**;
   it is not the separate 0.2.6 deliberate-fault rollback image.
2. Package the signed app with `python tools/ota_package.py signed-app.bin --output update.sota`.
3. In the App explicitly open the LAN maintenance window on a network that the
   computer can reach. It displays the real device IPv4 and current window code.
4. On the computer open `http://<device-ip>/`, select the local `.sota`, input
   the current code in the masked field and explicitly install; or:

   ```sh
   python tools/ota_upload.py update.sota                # offline plan only
   python tools/ota_upload.py update.sota --host 192.168.1.80 --install-and-restart
   ```

   The CLI prompts for the code hidden at the terminal; no secret argv, cookies,
   persistent authorization, environment extraction, proxy, redirect or retry.
   It sends exactly one bounded POST. A lost response means unknown, not a reason
   to upload again. AP requires an explicit `--ap-fallback` instead of LAN token.
5. A 200 response means image committed for restart. Reconnect and verify the
   actual higher firmware version and VALID startup before crediting an upgrade.

The LAN server requires `X-Satori-Window` in constant-time comparison, scoped to
this RAM window. The IP URL contains no code. The existing manifest, size,
board/chip, higher-version, SHA, SDK RSA verification, inactive-slot-only write,
commit mutex and boot rollback remain the same. Ordinary trusted-LAN HTTP is
not TLS and this prototype does not claim credential confidentiality from a
hostile LAN observer. AP uses its one-window WPA2 credentials as before.

## Local validation and deployment boundary

Host tests exercise wire limits, duplicate/conflicting IDs, cross-mode admission,
header/token policy and single-upload/no retry. Existing BLE, startup and OTA
sink tests remain required, alongside Flutter protocol/session/UI tests and all
four firmware profiles. Actual Wi-Fi/DHCP/HTTP/expiry are still hardware
acceptance items, not proven by these local tests.

The installed signed 0.2.5 has AP OTA but **no 0008/LAN provisioning**. The App
shows unsupported for that firmware, rather than sending credentials or claiming
it is configured. First deployment of LAN support therefore needs a separately
authorized initial update through the existing AP or protected app-only USB
route; subsequent updates are computer-to-device on LAN.

This phase does not authorize firmware flashing, signing a release, phone
installation, network changes or reuse of stored Wi-Fi secrets. Actual validation
needs explicit approval of the target network and initial support-firmware
deployment. Users enter the password in the App. The current satori default
network is unchanged; no new device connection is part of these tests.
