"""Explicit factory-to-dual migration. No device access during planning/import."""
import signal
import struct
from . import ota_records as records
from .runtime import Rejected, digest
from .usb_upgrade import FLASH, SLOT, OTADATA, validate_candidate, Plan as UpdatePlan

REQUIRED_FLAGS = {
    'CONFIG_IDF_TARGET': '"esp32c3"',
    'CONFIG_ESPTOOLPY_FLASHSIZE_4MB': 'y',
    'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME': '"partitions.dual_ota.csv"',
    'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE': 'y',
    'CONFIG_SATORI_DUAL_OTA_BOOT_CONFIRM': 'y',
    'CONFIG_SATORI_WIFI_OTA_PROTOTYPE': 'y',
    'CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT': 'y',
    'CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT': 'y',
    'CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME': 'y',
    'CONFIG_SECURE_BOOT': 'n', 'CONFIG_SECURE_FLASH_ENC_ENABLED': 'n',
    'CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK': 'n',
    'CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES': 'n',
    'CONFIG_SECURE_SIGNED_ON_BOOT_NO_SECURE_BOOT': 'n',
    'CONFIG_SECURE_SIGNED_ON_BOOT': 'n',
}

def build_proof(build, sdk_version):
    """Export only public build flags/hashes; no generated config is exported."""
    values = {key: 'n' for key in REQUIRED_FLAGS}
    for line in (build / 'sdkconfig').read_text().splitlines():
        key, separator, value = line.partition('=')
        if separator and key in values: values[key] = value
    files = {'unsigned_app': 'app.bin', 'bootloader': 'bootloader/bootloader.bin',
             'partition_table': 'partition_table/partition-table.bin'}
    return {'schema': 1, 'sdk_version': sdk_version, 'flags': values,
            'sha256': {name: digest((build / path).read_bytes()) for name, path in files.items()}}

class Plan:
    def __init__(self, candidate, old_app, old_boot, old_table, new_boot, new_table, unsigned_app, proof, verify):
        if records.classify_table(old_table) != 'factory':
            raise Rejected('Migration accepts exact factory layout only; already migrated/unknown devices must not migrate')
        records.require_dual(new_table)
        if proof.get('schema') != 1 or proof.get('sdk_version') != 'ESP-IDF v5.5.4' or proof.get('flags') != REQUIRED_FLAGS:
            raise Rejected('Reviewed rollback/signature build configuration required')
        expected = {'unsigned_app': digest(unsigned_app), 'bootloader': digest(new_boot), 'partition_table': digest(new_table)}
        body_size = (len(unsigned_app) + 4095) // 4096 * 4096
        if len(unsigned_app) < 336 or len(candidate) != body_size + 4096 or candidate[:len(unsigned_app)] != unsigned_app or candidate[len(unsigned_app):body_size] != b'\xff' * (body_size-len(unsigned_app)) or proof.get('sha256') != expected:
            raise Rejected('Build proof/image identities differ')
        self.metadata = validate_candidate(candidate, digest(candidate), verify)
        if len(old_app) < 336 or len(old_app) > SLOT or old_app[0] != 0xe9 or struct.unpack_from('<H', old_app, 12)[0] != 5 or struct.unpack_from('<I', old_app, 32)[0] != 0xabcd5432:
            raise Rejected('Old reviewed C3 image must fit ota_0')
        if not old_boot or len(old_boot) > 0x8000 or len(new_boot) > 0x8000 or new_boot[:4] != old_boot[:4] or new_boot[0] != 0xe9 or new_boot[2:4] != bytes([2, 0x2f]) or struct.unpack_from('<H', new_boot, 12)[0] != 5:
            raise Rejected('Unsupported bootloader/flash geometry')
        if candidate[36:40] != old_app[36:40] or candidate[80:112] != old_app[80:112]:
            raise Rejected('Project/security version differs')
        self.old_app = old_app; self.old_boot = records.pad_region(old_boot); self.old_table = records.pad_region(old_table)
        self.new_boot = records.pad_region(new_boot); self.new_table = records.pad_region(new_table)
        self.image = records.pad_region(candidate); self.target = 1; self.start = 0x180000
        selector = bytearray(b'\xff' * 8192)
        selector[:32] = records.ota_record(1, 2); selector[4096:4128] = records.ota_record(2, 0)
        self.selector = bytes(selector)
    def public(self):
        return {'offline_plan': True, 'from_layout': 'factory', 'to_layout': 'dual-ota',
                'candidate_version': self.metadata['version'], 'device_accessed': False,
                'writes': ['inactive app 0x180000', 'otadata 0x303000', 'rollback bootloader 0x0', 'partition table 0x8000 LAST'],
                'old_app_nvs_phy_config_preserved': True, 'automatic_migration': False,
                'bootloader_partition_partial_write_recoverable_by_app_rollback': False}
    def validate(self, first, second):
        if len(first) != FLASH or first != second: raise Rejected('Fresh full double backup mismatch')
        if first[:len(self.old_boot)] != self.old_boot or first[0x8000:0x9000] != self.old_table:
            raise Rejected('Actual old bootloader/table differs from reviewed reference')
        if records.classify_table(first[0x8000:0x8c00]) != 'factory': raise Rejected('Actual layout is not supported factory')
        if first[0x10000:0x10000+len(self.old_app)] != self.old_app: raise Rejected('Old application differs from reviewed reference')
        for start, size in ((self.start, SLOT), (OTADATA, 8192)):
            if first[start:start+size] != b'\xff' * size: raise Rejected('Migration staging/selector contains data; refuse overwrite')
        return True
    def boot_ok(self, lines, pending):
        return UpdatePlan.boot_ok(self, lines, pending)
    def postboot_match(self, actual, expected):
        if len(actual) != FLASH: return False
        # Runtime NVS writes and NEW->VALID are the only allowed postboot changes.
        out = bytearray(expected)
        out[0x9000:0xf000] = actual[0x9000:0xf000]
        out[OTADATA+4096:OTADATA+4128] = records.ota_record(2, 2)
        return bytes(out) == actual and records.selected_slot(actual[OTADATA:OTADATA+8192]) == 1

def execute(plan, io, save, backup):
    result = {'success': False, 'writes_started': False, 'boot_regions_write_started': False,
              'automatic_recovery_attempted': False, 'manual_recovery_required': False, 'port_closed': False}
    expected = None; old_sigint = None
    def phase(name): result['phase'] = name; save(result)
    try:
        phase('open_rom'); io.open()
        phase('backup_1'); baseline = io.read(0, FLASH); backup(1, baseline)
        phase('backup_2'); second = io.read(0, FLASH); backup(2, second)
        phase('validate_factory'); plan.validate(baseline, second); expected = bytearray(baseline)
        result['backup_sha256'] = digest(baseline); save(result)
        steps = [('inactive_app', plan.start, plan.image), ('fallback_record', OTADATA, plan.selector[:4096]),
                 ('candidate_record', OTADATA+4096, plan.selector[4096:]),
                 ('rollback_bootloader', 0, plan.new_boot), ('activate_partition_table_LAST', 0x8000, plan.new_table)]
        for name, address, data in steps:
            phase('write_' + name); result['writes_started'] = True
            if address in (0, 0x8000): result['boot_regions_write_started'] = True
            save(result); io.write(address, data); expected[address:address+len(data)] = data
            phase('verify_' + name)
            if io.read(0, FLASH) != bytes(expected): raise Rejected('Stage full readback/protected-region mismatch')
        phase('first_pending_boot'); io.boot()
        if not plan.boot_ok(io.observe(26), True): raise Rejected('Candidate startup/identity confirmation missing')
        phase('verify_actual_valid'); io.enter(); actual = io.read(0, FLASH)
        if not plan.postboot_match(actual, bytes(expected)): raise Rejected('Actual VALID or protected-region comparison failed')
        phase('second_valid_boot'); io.boot()
        if not plan.boot_ok(io.observe(20), False): raise Rejected('Second VALID startup not confirmed')
        result['success'] = True; phase('complete')
    except (Exception, KeyboardInterrupt) as error:
        old_sigint = signal.getsignal(signal.SIGINT); signal.signal(signal.SIGINT, signal.SIG_IGN)
        result['failed_phase'] = result.get('phase'); result['error_type'] = type(error).__name__
        result['interrupted'] = isinstance(error, KeyboardInterrupt)
        result['manual_recovery_required'] = result['writes_started']
        # Never rewrite boot/table on failure. A partial boot region is outside
        # the guarantees of app rollback. Preserve two snapshots and the journal.
        if not result['writes_started'] and getattr(io, 'opened', False):
            try: io.boot(); result['unchanged_boot_requested'] = True
            except Exception: result['unchanged_boot_requested'] = False
    finally:
        try: io.close(); result['port_closed'] = True
        except Exception: result['port_closed'] = False
        result['left_in_rom'] = getattr(io, 'in_rom', None)
        try: save(result)
        finally:
            if old_sigint is not None: signal.signal(signal.SIGINT, old_sigint)
    return result
