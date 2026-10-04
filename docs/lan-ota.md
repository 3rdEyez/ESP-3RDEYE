# LAN OTA (local 0.2.8 experience implementation)

Normal control remains BLE. Networking starts only from an explicit, secure bonded
maintenance OPEN. This local implementation is unsigned and uninstalled; the
previous reviewed 0.2.7 artifact stays frozen and separate.

## Network choice and compatibility

- Schema 1 on characteristic 0008 configures a temporary network; nothing is saved.
- Schema 3 has the same credential-bearing OPEN layout and explicitly remembers
  that network after obtaining IP. The App defaults its “记住此网络” checkbox off.
  Reconfiguring and opting in replaces the remembered network; temporary use does
  not overwrite it. Only device NVS namespace `maint_net`, single blob `network`,
  is used; old config.ini/hotspot fields and compile-time credentials are never
  imported. No SSID/password is returned in status or persisted by the App.
- Schema 2 is a credential-free, exactly 12-byte OPEN using that remembered network.
  Reuse never happens on boot, BLE connection or ordinary control, only user OPEN.
- CLOSE remains schema 1, action 2, no credentials. Schema 2/3 cannot encode CLOSE.

Optional secure characteristic `4d89f6a0-73b9-4f14-9d3e-63b2145a0009` reads four
bytes: schema=1, flags=3 (remember+reuse), saved-present=0/1, reserved=0.
DeviceInfo capability bits and control protocol 1.2 do not change, avoiding old
App decoder rejection of bit 0x200. The new App hides new choices if this optional
feature is absent or unreadable. Old firmware keeps its original expiry; the UI
uses device remaining time instead of claiming ten minutes.

NVS is local persistent storage; this feature does not enable encryption. A save
error is uncertain: the SDK may already have written its single blob before
commit reports a failure. Clients refresh non-secret saved-present metadata and
show uncertainty, rather than promise old-network rollback. Same-size corrupt
blobs can report present; reuse validates schema, bounds and termination and
fails closed. Temporary RAM copies are wiped on success and failure.

## Window lifecycle

Both LAN and AP maintenance have a ten-minute idle deadline from explicit OPEN,
including the bounded initial connection. Public GETs, reads, duplicate requests
and reconnections do not extend it. Once an authorized POST starts, a separate
120-second total upload deadline applies; crossing the original idle deadline
does not abort that upload. Flag/timestamp/clock reads have an explicit order.
Window status reports the current phase deadline.

Motion stops before networking. STA connection has a 20-second bound, at most
two retries before readiness, and no reconnect after established Wi-Fi loss.
BLE authorization and the original connection epoch must remain valid throughout;
BLE loss, Wi-Fi loss, errors or deadlines stop the window and clean owned resources.
The user may explicitly close; a committed image cannot be falsely cancelled.
Exit never restores old motion or ARM. “结束拍摄” reminds the user to switch off
physical power: software pause/disconnect does not cut servo or board power.

## Wire details

0008 action 1=OPEN, 2=CLOSE at byte1; request_id LE32 at2 nonzero, window_id LE32
at6 zero for OPEN/current for CLOSE; SSID/password lengths at10/11, data at12.
SSID: 1–32 bytes without controls/NUL. Password: 8–63 printable ASCII, personal
2.4GHz WPA2 network. Schema2 requires zero lengths and no payload.

Read remains schema1/state0..6/result0..6/detail0..6; IDs/remaining_ms at4/8/12,
IPv4 at16..19, token length at20, reserved zeros21..23, token at24. Details:
0 none, 1 config, 2 connection timeout, 3 Wi-Fi failure, 4 link lost, 5 no usable
remembered network, 6 saving unconfirmed. Only Open/Uploading expose IP and
32-character random hex bearer to the authorized BLE client. No credentials.
Schema participates in RAM replay fingerprints; same request ID with changed
schema/credentials is rejected, not another save/open.

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
   actual installed firmware version and VALID startup before crediting an upgrade.

The LAN server requires `X-Satori-Window` in constant-time comparison, scoped to
this RAM window. The IP URL contains no code. The existing manifest, size,
board/chip, descriptor-version consistency, SHA, SDK RSA verification, inactive-slot-only write,
commit mutex and boot rollback remain the same. Ordinary trusted-LAN HTTP is
not TLS and this prototype does not claim credential confidentiality from a
hostile LAN observer. AP uses its one-window WPA2 credentials as before.

## Local validation and deployment boundary

Host tests exercise wire limits, duplicate/conflicting IDs, cross-mode admission,
header/token policy and single-upload/no retry. Existing BLE, startup and OTA
sink tests remain required, alongside Flutter protocol/session/UI tests and all
four firmware profiles. Actual Wi-Fi/DHCP/HTTP/expiry are still hardware
acceptance items, not proven by these local tests.

The most recently verified installed baseline is normal 0.2.6: it supports LAN
but remains temporary-only with its original short deadline. New 0.2.8 behavior
has not been signed or deployed. App merging, phone installation and real-device
acceptance are deferred by the user.

Desktop `lan-desktop-provision.py --terminal --remember-network` explicitly saves
LKl on supported 0.2.8; `--terminal --saved-network` reuses it without asking for
its password. Both require personal CONNECT and keep the authorization code
private unless the user explicitly requests SHOW. The one-shot upload workflow verifies package metadata and the established
public-key signature before password entry; it accepts any valid application
version and can be combined with these flags. It requires one ordinary local
INSTALL confirmation, not a special same-version/downgrade confirmation. No permanent service or listener is added.

This local phase performs no signing, deployment, phone installation, network
configuration, or access to real stored credentials. Users must personally enter
and submit credentials when eventual hardware acceptance is authorized.

## Application version policy

Application versions are metadata, not an ordering or duplicate-package gate.
Same-version images (identical or different bytes), lower versions and higher
versions follow the same installation path. Versions must still be three numeric
components (0..65535, no leading zeros), and the manifest version must match the
stored ESP-IDF descriptor. Signature, board/chip, SHA including Flash readback,
project_name, secure_version equality, current VALID slot and inactive-target
checks remain. A previous committed window cannot be reused: each intentional
installation requires a fresh explicit maintenance window.

App compatibility remains firmware 0.2.x, protocol1.2 and required capabilities;
patch releases may differ. Current local configs do not enable SDK application
anti-rollback; no eFuse state was read or modified. Secure-version equality is
independent of application version ordering and is intentionally unchanged.
