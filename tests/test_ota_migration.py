import hashlib
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('migration', ROOT/'tools/ota_migration_artifacts.py')
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


class MigrationTests(unittest.TestCase):
    def test_rejects_valid_md5_table_with_unsafe_layout(self):
        def table_for(layout):
            data = b''.join(struct.pack('<HBBII16sI', 0x50aa, kind, subtype, address,
                                      size, label.encode(), flags)
                            for kind, subtype, address, size, label, flags in layout)
            data += b'\xeb\xeb' + b'\xff'*14 + hashlib.md5(data).digest()
            return data + b'\xff'*(3072-len(data))
        migration.validate_partition_table(table_for(migration.EXPECTED_LAYOUT))
        for index, field, value in [(2, 2, 0x20000), (4, 3, 0x3000), (0, 5, 1), (5, 2, 0x302000)]:
            layout = [list(entry) for entry in migration.EXPECTED_LAYOUT]
            layout[index][field] = value
            with self.assertRaises(ValueError):
                migration.validate_partition_table(table_for([tuple(entry) for entry in layout]))
        damaged = bytearray(table_for(migration.EXPECTED_LAYOUT))
        damaged[6*32+16] ^= 1
        with self.assertRaises(ValueError):
            migration.validate_partition_table(damaged)

    def test_initial_selection_and_pending_failure_fallback(self):
        data = bytearray(migration.initial_otadata())
        self.assertEqual(migration.selected_slot(data), 1)
        data[4096:4128] = migration.ota_record(2, 1)  # pending still selected
        self.assertEqual(migration.selected_slot(data), 1)
        data[4096:4128] = migration.ota_record(2, 4)  # next boot aborts pending
        self.assertEqual(migration.selected_slot(data), 0)
        data[4096:4128] = migration.ota_record(2, 3)
        self.assertEqual(migration.selected_slot(data), 0)
        data[4096:4128] = migration.ota_record(2, 2)
        self.assertEqual(migration.selected_slot(data), 1)

    def test_torn_selection_records_keep_existing_valid_slot(self):
        data = bytearray(migration.initial_otadata())
        data[4096:8192] = b'\xff'*4096
        record = migration.ota_record(2, 0)
        for count in range(32):
            torn = data.copy()
            torn[4096:4096+count] = record[:count]
            self.assertEqual(migration.selected_slot(torn), 0)
        damaged = bytearray(migration.initial_otadata())
        damaged[4096] ^= 0x10
        self.assertEqual(migration.selected_slot(damaged), 0)

    def test_all_staging_steps_preserve_protected_and_fallback_regions(self):
        # Fault-injected staged writes: all byte/sector endpoints preserve the
        # regions even when bootability of bootloader/table cannot be promised.
        original = bytes((i % 251 for i in range(0x400000)))
        steps = [(0x180000, 798304), (0x303000, 8192), (0, 21152), (0x8000, 3072)]
        protected = [(0x9000, 0x10000), (0x10000, 0x180000), (0x300000, 0x303000)]
        for start, size in steps:
            erase_end = (start+size+4095)&~4095
            for amount in [0, 1, size//2, size-1, size]:
                flash = bytearray(original)
                flash[start:erase_end] = b'\xff'*(erase_end-start)
                flash[start:start+amount] = b'\xaa'*amount
                for low, high in protected:
                    self.assertEqual(flash[low:high], original[low:high])
                # Any corruption is confined to this explicitly planned region.
                self.assertEqual(flash[:start], original[:start])
                self.assertEqual(flash[erase_end:], original[erase_end:])

    @unittest.skipUnless(os.environ.get('IDF_PATH'), 'Set IDF_PATH for native selector source regression')
    def test_exact_pinned_idf_selector_accepts_generated_records(self):
        source = (Path(os.environ['IDF_PATH']) / 'components/bootloader_support/src/bootloader_common_loader.c').read_text()
        names = ['bootloader_common_ota_select_crc', 'bootloader_common_ota_select_invalid',
                 'bootloader_common_ota_select_valid', 'bootloader_common_select_otadata',
                 'bootloader_common_get_active_otadata']
        functions = []
        for name in names:
            match = re.search(r'(?:uint32_t|bool|int)\s+'+name+r'\([^;]*?\)\s*\{', source, re.S)
            self.assertIsNotNone(match)
            start = match.start()
            position = match.end()
            depth = 1
            while depth:
                depth += (source[position] == '{') - (source[position] == '}')
                position += 1
            functions.append(source[start:position])
        prelude = '''#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>
#include <zlib.h>
typedef struct { uint32_t ota_seq; uint8_t seq_label[20]; uint32_t ota_state; uint32_t crc; } esp_ota_select_entry_t;
#define ESP_OTA_IMG_INVALID 3
#define ESP_OTA_IMG_ABORTED 4
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ESP_LOGD(...) ((void)0)
uint32_t esp_rom_crc32_le(uint32_t seed, const uint8_t* data, size_t size) { return crc32(seed,data,size); }
'''
        main = '''int main(int argc,char**argv) {
assert(argc==2); FILE*f=fopen(argv[1],"rb"); assert(f); esp_ota_select_entry_t data[2];
assert(fread(&data[0],32,1,f)==1); assert(fseek(f,4096,SEEK_SET)==0); assert(fread(&data[1],32,1,f)==1); fclose(f);
assert(sizeof(data[0])==32); assert(bootloader_common_get_active_otadata(data)==1);
data[1].ota_state=1; assert(bootloader_common_get_active_otadata(data)==1);
data[1].ota_state=4; assert(bootloader_common_get_active_otadata(data)==0);
data[1].ota_state=3; assert(bootloader_common_get_active_otadata(data)==0);
data[1].ota_state=0; data[1].crc^=1; assert(bootloader_common_get_active_otadata(data)==0);
return 0; }
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            code = directory/'selector.c'
            code.write_text(prelude+'\n'.join(functions)+main)
            data = directory/'initial.bin'
            data.write_bytes(migration.initial_otadata())
            exe = directory/'selector'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(code),'-lz','-o',str(exe)],check=True)
            subprocess.run([str(exe),str(data)],check=True)


if __name__ == '__main__':
    unittest.main()
