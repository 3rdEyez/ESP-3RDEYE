"""Board layout and selection helpers only; no fault deployment path."""
import binascii
import hashlib
import struct

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




def records(data):
    if len(data) != 8192:
        raise ValueError('Invalid selector size')
    return [decode_record(data[i:i + 32]) for i in (0, 4096)]


def selection(old, target):
    rows = records(old)
    eligible = [row for row in rows if row[2] and row[0] not in (0, 0xffffffff)]
    if not eligible or target not in (0, 1):
        raise ValueError('No eligible selector')
    sequence = max(row[0] for row in eligible) + 1
    if (sequence - 1) % 2 != target:
        sequence += 1
    if sequence >= 0xfffffffe:
        raise ValueError('Sequence wrap requires separate review')
    active_index = rows.index(max(eligible, key=lambda row: row[0]))
    output = bytearray(old)
    index = 1 - active_index
    output[index * 4096:(index + 1) * 4096] = b'\xff' * 4096
    output[index * 4096:index * 4096 + 32] = ota_record(sequence, 0)
    return bytes(output), sequence


def pad_region(blob):
    return blob + b'\xff' * ((-len(blob)) % 4096)
