"""All data and transports are synthetic; no device, credentials or signing."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from satori_tools import migration, ota_records as records
from satori_tools.runtime import Rejected, digest
import satori_dev as cli
from test_dev_tools import image, FakeRom

def table(layout):
    rows=b''.join(struct.pack('<HBBII16sI',0x50aa,k,s,a,n,l.encode().ljust(16,b'\0'),f) for k,s,a,n,l,f in layout)
    return (rows+b'\xeb\xeb'+b'\xff'*14+hashlib.md5(rows).digest()).ljust(3072,b'\xff')
def boot(marker):
    data=bytearray(512);data[:4]=b'\xe9\x01\x02\x2f';struct.pack_into('<H',data,12,5);data[200]=marker;return bytes(data)
def fixture():
    candidate=image('0.2.8');old=image('0.2.4',2);unsigned=candidate[:4096]
    oldboot=boot(1);newboot=boot(2);oldtable=table(records.FACTORY_LAYOUT);newtable=table(records.EXPECTED_LAYOUT)
    proof={'schema':1,'sdk_version':'ESP-IDF v5.5.4','flags':dict(migration.REQUIRED_FLAGS),
           'sha256':{'unsigned_app':digest(unsigned),'bootloader':digest(newboot),'partition_table':digest(newtable)}}
    plan=migration.Plan(candidate,old,oldboot,oldtable,newboot,newtable,unsigned,proof,lambda:True)
    flash=bytearray(b'\xff'*migration.FLASH);flash[:len(plan.old_boot)]=plan.old_boot
    flash[0x8000:0x9000]=plan.old_table;flash[0x10000:0x10000+len(old)]=old
    flash[0x9000:0x9010]=b'synthetic-nvs-00';flash[0x300000:0x300010]=b'synthetic-config'
    return plan,bytes(flash),proof
class Tests(unittest.TestCase):
    def test_factory_dual_unknown_encrypted_and_oversized_classification(self):
        self.assertEqual(records.classify_table(table(records.FACTORY_LAYOUT)),'factory')
        self.assertEqual(records.classify_table(table(records.EXPECTED_LAYOUT)),'dual-ota')
        for layout in (records.EXPECTED_LAYOUT[:-1],[(k,s,a,n,l,1) for k,s,a,n,l,f in records.EXPECTED_LAYOUT]):
            self.assertEqual(records.classify_table(table(layout)),'unknown')
        self.assertEqual(records.classify_table(table(records.EXPECTED_LAYOUT)+b'\xff'*4096),'unknown')
        for data in (table(records.FACTORY_LAYOUT),b'bad'):
            with self.assertRaisesRegex(Rejected,'migrate_layout'):records.require_dual(data)
    def test_success_preserves_old_app_and_private_regions(self):
        plan,flash,_=fixture();rom=FakeRom(plan,flash);journal=[];backups=[]
        result=migration.execute(plan,rom,lambda meta:journal.append(dict(meta)),lambda n,b:backups.append((n,b)))
        self.assertTrue(result['success'],result)
        self.assertEqual([a for a,n in rom.writes],[0x180000,0x303000,0x304000,0,0x8000])
        self.assertEqual(rom.read(0x10000,0x170000),flash[0x10000:0x180000])
        for a,n in ((0x9000,0x6000),(0xf000,0x1000),(0x300000,0x2800)):
            self.assertEqual(rom.read(a,n),flash[a:a+n])
        self.assertEqual(backups,[(1,flash),(2,flash)]);self.assertTrue(journal[-1]['success'])
    def test_double_backup_data_space_and_old_image_mismatch_refused(self):
        plan,flash,_=fixture()
        with self.assertRaises(Rejected):plan.validate(flash,flash[:-1])
        for address in (0,0x8000,0x10000,0x180000,0x303000):
            changed=bytearray(flash);changed[address]^=1
            with self.assertRaises(Rejected):plan.validate(bytes(changed),bytes(changed))
    def test_already_migrated_unknown_or_bad_build_proof_refused(self):
        plan,_,proof=fixture()
        for oldtable in (table(records.EXPECTED_LAYOUT),b'bad'):
            with self.assertRaises(Rejected):migration.Plan(plan.image,plan.old_app,boot(1),oldtable,boot(2),table(records.EXPECTED_LAYOUT),plan.image[:4096],proof,lambda:True)
        bad=json.loads(json.dumps(proof));bad['flags']['CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE']='n'
        with self.assertRaises(Rejected):migration.Plan(plan.image,plan.old_app,boot(1),table(records.FACTORY_LAYOUT),boot(2),table(records.EXPECTED_LAYOUT),plan.image[:4096],bad,lambda:True)
    def test_signature_boot_geometry_and_oversized_old_app_refused(self):
        plan,_,proof=fixture()
        args=[plan.image,plan.old_app,boot(1),table(records.FACTORY_LAYOUT),boot(2),table(records.EXPECTED_LAYOUT),plan.image[:4096],proof,lambda:False]
        with self.assertRaises(Rejected):migration.Plan(*args)
        args[-1]=lambda:True;args[1]=plan.old_app+b'\xff'*migration.SLOT
        with self.assertRaises(Rejected):migration.Plan(*args)
        args[1]=plan.old_app;args[4]=b'bad'
        with self.assertRaises((Rejected,struct.error)):migration.Plan(*args)
    def test_each_interruption_records_stage_and_never_rewrites_boot(self):
        for stage in (0x180000,0x303000,0x304000,0,0x8000):
            plan,flash,_=fixture()
            class Interrupted(FakeRom):
                def write(self,address,data):
                    self.writes.append((address,len(data)))
                    self.flash[address:address+1024]=data[:1024]
                    if address==stage:raise KeyboardInterrupt()
                    self.flash[address:address+len(data)]=data
            rom=Interrupted(plan,flash);journal=[]
            result=migration.execute(plan,rom,lambda meta:journal.append(dict(meta)),lambda n,b:None)
            self.assertFalse(result['success']);self.assertTrue(result['manual_recovery_required'])
            self.assertFalse(result['automatic_recovery_attempted']);self.assertEqual(rom.boots,0)
            self.assertTrue(result['port_closed']);self.assertTrue(result['left_in_rom'])
            self.assertEqual(rom.read(0x10000,0x170000),flash[0x10000:0x180000])
            self.assertIn('write_',result['failed_phase'])
    def test_readback_failure_never_activates_partition_table(self):
        plan,flash,_=fixture()
        class Corrupt(FakeRom):
            def write(self,address,data):
                super().write(address,data)
                if address==0x180000:self.flash[address]^=1
        rom=Corrupt(plan,flash);result=migration.execute(plan,rom,lambda meta:None,lambda n,b:None)
        self.assertFalse(result['success']);self.assertEqual(rom.writes,[(0x180000,len(plan.image))])
    def test_default_migration_plan_never_creates_transport(self):
        args=cli.parser().parse_args(['migrate-layout','--config','synthetic'])
        plan,_,_=fixture()
        with patch.object(cli,'configuration',return_value={}),patch.object(cli,'migration_plan',return_value=plan),patch.object(cli,'execute_usb',side_effect=AssertionError):
            self.assertFalse(cli.run(args)['device_accessed'])
    def test_short_prefix_build_proof_and_boot_signature_mode_refused(self):
        plan,_,proof=fixture()
        for unsigned in (b'',b'\xe9',plan.image[:336]):
            fake=json.loads(json.dumps(proof));fake['sha256']['unsigned_app']=digest(unsigned)
            with self.assertRaises(Rejected):migration.Plan(plan.image,plan.old_app,boot(1),table(records.FACTORY_LAYOUT),boot(2),table(records.EXPECTED_LAYOUT),unsigned,fake,lambda:True)
        bad=json.loads(json.dumps(proof));bad['flags']['CONFIG_SECURE_SIGNED_ON_BOOT']='y'
        with self.assertRaises(Rejected):migration.Plan(plan.image,plan.old_app,boot(1),table(records.FACTORY_LAYOUT),boot(2),table(records.EXPECTED_LAYOUT),plan.image[:4096],bad,lambda:True)
    def test_postboot_phy_config_and_fallback_reject_changes(self):
        plan,flash,_=fixture();rom=FakeRom(plan,flash)
        migration.execute(plan,rom,lambda value:None,lambda n,b:None)
        expected=bytearray(flash)
        for a,data in ((plan.start,plan.image),(migration.OTADATA,plan.selector),(0,plan.new_boot),(0x8000,plan.new_table)):expected[a:a+len(data)]=data
        actual=rom.read(0,migration.FLASH)
        self.assertTrue(plan.postboot_match(actual,bytes(expected)))
        for address in (0xf000,0x300000,0x10000,migration.OTADATA):
            changed=bytearray(actual);changed[address]^=1
            self.assertFalse(plan.postboot_match(bytes(changed),bytes(expected)))
    def test_normal_usb_actual_factory_backup_refused_before_write(self):
        from test_dev_tools import setup
        from satori_tools import usb_upgrade
        plan,flash=setup();actual=bytearray(flash);actual[0x8000:0x9000]=records.pad_region(table(records.FACTORY_LAYOUT))
        rom=FakeRom(plan,bytes(actual));result=usb_upgrade.execute(plan,rom)
        self.assertFalse(result['success']);self.assertEqual(rom.writes,[])
        self.assertIn('migrate_layout',result['guidance'])
    def test_execution_requires_private_tty_and_personal_confirmation(self):
        plan,_,_=fixture();args=cli.parser().parse_args(['migrate-layout','--config','synthetic','--execute'])
        with patch.object(cli,'configuration',return_value={}),patch.object(cli,'migration_plan',return_value=plan),patch.object(cli,'execute_usb',side_effect=AssertionError):
            with patch.object(sys.stdin,'isatty',return_value=False):
                with self.assertRaises(Rejected):cli.run(args)
            with patch.object(sys.stdin,'isatty',return_value=True),patch.object(sys.stdout,'isatty',return_value=True),patch.object(sys.stderr,'isatty',return_value=True),patch('builtins.input',return_value='cancel'):
                self.assertFalse(cli.run(args)['device_accessed'])
