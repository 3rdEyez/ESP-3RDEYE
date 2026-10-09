#!/usr/bin/env python3
"""Offline bundle generation and OTA selection checks. No device interfaces."""
import argparse
import binascii
import hashlib
import json
from pathlib import Path
import struct

OLD_APP_BYTES = 777440
OLD_APP_SHA = '1736e945a7d9dc8bbaef8bb974dde9657bb7bb5d0c5a44be617663d84b224283'
BACKUP_SHA = '2a65887dc7744424262ab5c01dfec0ec5c6dc41794fcdcbba14a259faaf0ac5f'
SLOT_BYTES = 0x170000
EXPECTED_LAYOUT = [
    (1, 2, 0x9000, 0x6000, 'nvs', 0),
    (1, 1, 0xf000, 0x1000, 'phy_init', 0),
    (0, 16, 0x10000, 0x170000, 'ota_0', 0),
    (0, 17, 0x180000, 0x170000, 'ota_1', 0),
    (1, 6, 0x300000, 0x2800, 'config', 0),
    (1, 0, 0x303000, 0x2000, 'otadata', 0),
]


def validate_partition_table(table):
    entries = []
    for offset in range(0, len(table), 32):
        record = table[offset:offset+32]
        if record[:2] == b'\xeb\xeb':
            if record[16:32] != hashlib.md5(table[:offset]).digest():
                raise ValueError('Partition table MD5 mismatch')
            if entries != EXPECTED_LAYOUT or any(b != 255 for b in table[offset+32:]):
                raise ValueError('Partition table differs from approved exact layout')
            return
        if len(record) != 32 or record[:2] != b'\xaa\x50':
            raise ValueError('Invalid partition table entry or missing MD5')
        _, kind, subtype, address, size, label, flags = struct.unpack('<HBBII16sI', record)
        entries.append((kind, subtype, address, size, label.rstrip(b'\0').decode('ascii'), flags))
    raise ValueError('Missing partition table MD5')


def ota_record(sequence, state):
    # Identical CRC convention to pinned IDF5.5.4 app_update/otatool.py.
    crc = binascii.crc32(struct.pack('<I', sequence), 0xffffffff) & 0xffffffff
    return struct.pack('<I20sII', sequence, b'\xff' * 20, state, crc)


def decode_record(record):
    sequence, label, state, crc = struct.unpack('<I20sII', record[:32])
    valid = sequence != 0xffffffff and state not in (3, 4) and crc == (
        binascii.crc32(struct.pack('<I', sequence), 0xffffffff) & 0xffffffff)
    return sequence, state, valid


def selected_slot(otadata):
    records = [decode_record(otadata[i:i+32]) for i in [0, 4096]]
    valid = [r for r in records if r[2]]
    if not valid:
        return 0  # no-factory fallback; caller must still verify image/state.
    return (max(valid, key=lambda r: r[0])[0] - 1) % 2


def initial_otadata():
    data = bytearray(b'\xff' * 8192)
    data[:32] = ota_record(1, 2)       # ota_0 existing, valid rollback baseline
    data[4096:4128] = ota_record(2, 0) # ota_1 new, bootloader sets pending
    return bytes(data)


def prepare(root, backup, output):
    original = backup.read_bytes()
    if len(original) != 0x400000 or hashlib.sha256(original).hexdigest() != BACKUP_SHA:
        raise ValueError('Latest approved double-read backup mismatch')
    old_app = original[0x10000:0x10000+OLD_APP_BYTES]
    if hashlib.sha256(old_app).hexdigest() != OLD_APP_SHA:
        raise ValueError('Rollback application differs from historical known-good image')
    if any(b != 255 for b in original[0x180000:0x2f0000]):
        raise ValueError('Staging slot contains unknown data; refuse to overwrite')
    if any(b != 255 for b in original[0x303000:0x305000]):
        raise ValueError('otadata target contains unknown data; refuse to overwrite')
    build = root / 'build/ble_dual_ota'
    config = (build / 'sdkconfig').read_text()
    required = ['CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y', 'CONFIG_SATORI_DUAL_OTA_BOOT_CONFIRM=y']
    if not all(line in config.splitlines() for line in required):
        raise ValueError('Rollback/startup validation build options missing')
    for key in ['CONFIG_SECURE_BOOT', 'CONFIG_SECURE_FLASH_ENC_ENABLED',
                'CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK', 'CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT']:
        if key + '=y' in config.splitlines():
            raise ValueError('Unauthorized trust/eFuse policy enabled')
    app = (build / 'app.bin').read_bytes()
    boot = (build / 'bootloader/bootloader.bin').read_bytes()
    table = (build / 'partition_table/partition-table.bin').read_bytes()
    if len(app) > SLOT_BYTES or len(boot) > 0x8000 or len(table) != 0xc00:
        raise ValueError('Artifacts exceed approved regions')
    validate_partition_table(table)
    if boot[:4] != original[:4] or boot[2:4] != bytes([2, 0x2f]):
        raise ValueError('Bootloader public flash geometry differs from verified original')
    # Reserve a 4KB RSA-v2 block and image padding for future design only.
    signed_reservation = ((len(app) + 4095) & ~4095) + 4096
    if signed_reservation > SLOT_BYTES:
        raise ValueError('Future aligned signature reservation does not fit')
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    data = [('ota1-app.bin', 0x180000, app),
            ('otadata-boot-new-with-valid-fallback.bin', 0x303000, initial_otadata()),
            ('rollback-bootloader.bin', 0, boot),
            ('dual-partition-table.bin', 0x8000, table)]
    artifacts = []
    for name, start, blob in data:
        path = output / name
        path.write_bytes(blob)
        path.chmod(0o600)
        artifacts.append({'file': name, 'address': hex(start), 'bytes': len(blob),
                          'erase_start': hex(start), 'erase_end_exclusive': hex((start+len(blob)+4095)&~4095),
                          'sha256': hashlib.sha256(blob).hexdigest()})
    metadata = {'source_backup_sha256': BACKUP_SHA, 'fallback_app_sha256': OLD_APP_SHA,
                'fallback_ota0_region_sha256': hashlib.sha256(original[0x10000:0x180000]).hexdigest(),
                'bootloader_public_flash_header_matches_original': True,
                'compiled_partition_table_exact_layout_and_md5_verified': True,
                'fallback_ota0_bytes': OLD_APP_BYTES, 'ota_slot_bytes': SLOT_BYTES,
                'future_signature_space_reservation_bytes': signed_reservation,
                'future_signature_margin_bytes': SLOT_BYTES-signed_reservation,
                'no_signed_network_OTA_implemented': True, 'device_writes_executed': False,
                'staging_slot_and_otadata_targets_are_erased': True,
                'steps_in_proposed_order': artifacts}
    (output / 'manifest.json').write_text(json.dumps(metadata, indent=2)+'\n')
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backup', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(Path(__file__).resolve().parent.parent, args.backup, args.output)
