"""Synthetic regression: never imports/constructs real device transports."""
import binascii
import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import satori_dev as cli
from satori_tools import artifacts, ota_records as records, usb_upgrade as usb
from satori_tools.runtime import Rejected, digest, private_output
import ota_package

def image(version, marker=1):
    data=bytearray(b'\xff'*8192);data[0]=0xe9
    struct.pack_into('<H',data,12,5);struct.pack_into('<I',data,32,0xabcd5432)
    data[48:80]=version.encode().ljust(32,b'\0');data[288:336]=ota_package.BOARD_TAG
    data[1000]=marker;data[4096:4098]=b'\xe7\x02'
    struct.pack_into('<I',data,4096+1196,binascii.crc32(data[4096:4096+1196])&0xffffffff)
    return bytes(data)
def table():
    data=b''.join(struct.pack('<HBBII16sI',0x50aa,k,s,a,n,label.encode().ljust(16,b'\0'),f) for k,s,a,n,label,f in records.EXPECTED_LAYOUT)
    return (data+b'\xeb\xeb'+b'\xff'*14+hashlib.md5(data).digest()).ljust(4096,b'\xff')
def setup(slot=1, version='0.2.7'):
    current=image('0.2.8',2);candidate=image(version)
    plan=usb.Plan(candidate,current,b'boot',table(),digest(candidate),lambda:True,slot,1-slot,digest(current))
    flash=bytearray(b'\xff'*usb.FLASH);flash[:4096]=plan.boot;flash[0x8000:0x9000]=plan.table
    start=0x10000+slot*usb.SLOT;flash[start:start+len(current)]=current
    flash[usb.OTADATA:usb.OTADATA+32]=records.ota_record(slot+1,2)
    return plan,bytes(flash)
class FakeRom:
    def __init__(self,plan,flash,fail=None):self.plan=plan;self.flash=bytearray(flash);self.opened=False;self.in_rom=False;self.writes=[];self.boots=0;self.fail=fail
    def open(self):self.opened=True;self.in_rom=True
    def enter(self):self.in_rom=True
    def read(self,start,size):return bytes(self.flash[start:start+size])
    def write(self,start,data):
        self.writes.append((start,len(data)));self.flash[start:start+len(data)]=data
        if self.fail=='selection' and start>=usb.OTADATA:self.fail=None;raise KeyboardInterrupt()
    def boot(self):self.in_rom=False;self.boots+=1
    def observe(self,seconds):
        raw=self.read(usb.OTADATA,8192);rows=records.records(raw);winner=max((r for r in rows if r[2]),key=lambda r:r[0]);slot=(winner[0]-1)%2;pending=winner[1]==0
        if pending:
            index=rows.index(winner);start=usb.OTADATA+4096*index;self.flash[start:start+32]=records.ota_record(winner[0],2)
        version=self.plan.metadata['version'] if slot==self.plan.target else self.plan.seed_metadata['version']
        return ['APP MAIN: Boot reset_reason=1','OTA_BOOT: build='+version,'OTA_BOOT: running_slot=%d state=%d'%(16+slot,1 if pending else 2),'Startup validation passed; slot confirmed']
    def close(self):self.opened=False
class Tests(unittest.TestCase):
    def test_default_device_commands_do_not_construct_transport(self):
        with tempfile.TemporaryDirectory() as directory:
            config=Path(directory)/'config.json';config.write_text('{}')
            for command in ('maintenance','status'):
                with patch.object(cli.maintenance,'BluezLink',side_effect=AssertionError):
                    self.assertTrue(cli.run(cli.parser().parse_args([command,'--config',str(config)]))['offline_plan'])
    def test_secret_config_nested_and_argv_redaction(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'c';path.write_text(json.dumps({'network':{'password':'synthetic-secret'}}))
            with self.assertRaises(Rejected):cli.configuration(path)
        stream=io.StringIO()
        with contextlib.redirect_stderr(stream),self.assertRaises(SystemExit):cli.parser().parse_args(['status','--password','synthetic-secret'])
        self.assertNotIn('synthetic-secret',stream.getvalue())
    def test_sign_dry_run_never_invokes_key_or_sdk(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);app=root/'image';pub=root/'public';out=root/'out'
            app.write_bytes(image('0.2.8'));pub.write_bytes(b'synthetic public')
            with patch.object(artifacts,'checked',side_effect=AssertionError):
                self.assertFalse(artifacts.sign_package(app,pub,digest(pub.read_bytes()),out)['signing_performed'])
            self.assertFalse(out.exists())
    def test_bad_public_trust_and_crypto_failure_never_package(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);app=root/'image';pub=root/'public';out=root/'out'
            app.write_bytes(image('0.2.8'));pub.write_bytes(b'synthetic public')
            with self.assertRaises(Rejected):artifacts.package_verified(app,pub,'0'*64,out)
            with patch.object(artifacts,'sdk_command',return_value=['fake']),patch.object(artifacts,'checked',side_effect=Rejected):
                with self.assertRaises(Rejected):artifacts.package_verified(app,pub,digest(pub.read_bytes()),out)
            self.assertFalse(out.exists())
    def test_private_output_rejects_checkout(self):
        with self.assertRaises(Rejected):private_output(Path(__file__).resolve().parents[1]/'.local/backup')
    def test_slot_version_matrix_preserves_fallback_during_erase(self):
        for slot in (0,1):
            for version in ('0.2.7','0.2.8','0.2.9'):
                plan,flash=setup(slot,version);old,new,seq=plan.validate_current(flash,flash)
                sector=usb.selection_sector(old,new);erased=bytearray(old);erased[sector:sector+4096]=b'\xff'*4096
                self.assertEqual(records.selected_slot(erased),slot)
                rom=FakeRom(plan,flash);result=usb.execute(plan,rom)
                self.assertTrue(result['success'],result)
                self.assertEqual([size for start,size in rom.writes if start>=usb.OTADATA],[4096])
    def test_interrupt_restores_one_sector_and_old_valid(self):
        plan,flash=setup();rom=FakeRom(plan,flash,'selection');result=usb.execute(plan,rom)
        self.assertFalse(result['success']);self.assertTrue(result['recovery_confirmed'],result)
        self.assertEqual(rom.read(usb.OTADATA,8192),flash[usb.OTADATA:usb.OTADATA+8192])
        self.assertEqual([size for start,size in rom.writes if start>=usb.OTADATA],[4096,4096])
    def test_backup_mismatch_no_write_and_active_slot_rejected(self):
        plan,flash=setup()
        with self.assertRaises(Rejected):plan.validate_current(flash,flash[:-1])
        with self.assertRaises(Rejected):usb.Plan(plan.image,plan.seed,b'boot',table(),digest(plan.image),lambda:True,1,1,digest(plan.seed))
    def test_extra_reset_panic_and_wrong_version_fail_confirmation(self):
        plan,flash=setup();rom=FakeRom(plan,flash);rom.boot();lines=rom.observe(1)
        # Baseline lines are valid for recovery, never candidate confirmation.
        self.assertTrue(plan.boot_ok(lines,False,plan.seed_metadata['version'],plan.current_slot))
        for bad in (lines+['rst:0x1','rst:0x1'],lines+['panic'],[s.replace('0.2.8','0.2.1') for s in lines]):
            self.assertFalse(plan.boot_ok(bad,False,plan.seed_metadata['version'],plan.current_slot))

class WirelessTests(unittest.TestCase):
    def test_single_post_and_user_cancel_or_expired_window(self):
        from satori_tools import wireless
        from types import SimpleNamespace
        blob,_=ota_package.encode_package(image('0.2.7'),'esp-idf-sbv2-rsa3072')
        value={'state':2,'result':0,'ack':7,'window':9,'remaining_ms':50000,'ip':'192.168.1.2','token':b'a'*32}
        session=SimpleNamespace(link=SimpleNamespace(read=lambda:value),rid=7,window=9)
        calls=[];outcome={}
        result=wireless.handoff(session,blob,lambda s:s,lambda _: 'INSTALL',outcome,lambda host:None,
                                lambda *a,**kw: calls.append(a) or {'submitted_for_restart':True})
        self.assertTrue(result['submitted_for_restart']);self.assertEqual(len(calls),1)
        for confirmation,remaining in [('cancel',50000),('INSTALL',44000)]:
            value['remaining_ms']=remaining
            with self.assertRaises(Rejected):wireless.handoff(session,blob,lambda s:s,lambda _:confirmation,{},lambda host:None,lambda *a,**k:self.fail('POST'))
    def test_post_failure_marked_unknown_never_retries(self):
        from satori_tools import wireless
        from types import SimpleNamespace
        blob,_=ota_package.encode_package(image('0.2.8'),'esp-idf-sbv2-rsa3072')
        value={'state':2,'result':0,'ack':7,'window':9,'remaining_ms':50000,'ip':'192.168.1.2','token':b'a'*32}
        session=SimpleNamespace(link=SimpleNamespace(read=lambda:value),rid=7,window=9);outcome={};calls=[]
        def upload(*a,**kw):kw['on_post_start']();calls.append(1);raise TimeoutError()
        with self.assertRaises(TimeoutError):wireless.handoff(session,blob,lambda s:s,lambda _:'INSTALL',outcome,lambda host:None,upload)
        self.assertTrue(outcome['post_started']);self.assertEqual(calls,[1])
    def test_probe_closes_connection_before_confirmation(self):
        from satori_tools import wireless
        from types import SimpleNamespace
        class Connection:
            sock=None
            def __init__(self,*a,**kw):self.closed=False
            def request(self,*a,**kw):self.requested=a
            def getresponse(self):return SimpleNamespace(status=200,read=lambda n:'<title>觉瞳固件升级</title>'.encode())
            def close(self):self.closed=True
        connection=Connection();wireless.probe('192.168.1.2',lambda *a,**kw:connection)
        self.assertTrue(connection.closed);self.assertEqual(connection.requested,('GET','/'))

class MaintenanceTests(unittest.TestCase):
    def test_modes_atomic_and_secret_buffers_wiped(self):
        from satori_tools import maintenance
        import threading
        def raw(state,ack=0,window=0):
            data=bytearray(24);data[0]=1;data[1]=state
            struct.pack_into('<III',data,4,ack,window,600000)
            if state==2:data[16:20]=b'\xc0\xa8\x01\x02';data[20]=32;data.extend(b'a'*32)
            return bytes(data)
        class Link:
            owned=False
            def __init__(self):self.command=None;self.commands=[]
            def connect(self):pass
            def version(self):return '0.2.8'
            def saved_feature(self,require_saved):return True
            def read(self):return raw(0) if self.command is None else raw(2,7,9)
            def write(self,command):self.command=command;self.commands.append(bytes(command))
            def disconnect(self):pass
        for remember,saved,wire_version in [(False,False,1),(True,False,3),(False,True,2)]:
            link=Link();session=maintenance.Session(link,b'synthetic-ssid',rid=7)
            password=bytearray() if saved else bytearray(b'synthetic-password')
            session.open(password,threading.Event(),remember,saved)
            self.assertEqual(len(link.commands),1);self.assertEqual(link.commands[0][0],wire_version)
            self.assertTrue(all(v==0 for v in password));self.assertTrue(all(v==0 for v in link.command))
            if saved:self.assertEqual(len(link.commands[0]),12)
    def test_protocol_gate_rejects_wrong_minor(self):
        from satori_tools import maintenance
        from types import SimpleNamespace
        link=maintenance.BluezLink('00:11:22:33:44:55','hci1')
        link.characteristic=lambda _:SimpleNamespace(ReadValue=lambda *a,**k:bytes([1,1])+bytes(18))
        with self.assertRaises(Rejected):link.version()

class CheckEntryTests(unittest.TestCase):
    def test_bare_flutter_resolves_path_and_all_commands_offline(self):
        from satori_tools import checks
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'pubspec.yaml').write_text('version: synthetic')
            with patch.dict('os.environ',{'IDF_PATH':'synthetic'}),patch.object(checks.shutil,'which',return_value='/synthetic/bin/flutter'),patch.object(checks.subprocess,'run',return_value=SimpleNamespace(returncode=0)) as run:
                self.assertTrue(checks.run(root,'flutter')['offline_checks_passed'])
                commands=[call.args[0] for call in run.call_args_list]
                self.assertIn(['/synthetic/bin/flutter','test','--no-pub'],commands)
                self.assertIn(['/synthetic/bin/dart','run','tool/check_ble_contract.dart'],commands)
                self.assertFalse(any('flash' in arg or 'sign_data' in arg for command in commands for arg in command))

class TerminalCleanupTests(unittest.TestCase):
    def test_cancel_closes_own_window_but_unknown_post_does_not_close(self):
        from types import SimpleNamespace
        for posted in (False,True):
            actions=[];stream=io.StringIO()
            class Session:
                connected=True;ready=None
                link=SimpleNamespace(disconnect=lambda:actions.append('disconnect'))
                def open(self,*a):return {'state':2,'result':0,'detail':0,'remaining_ms':600000,'ip':'192.168.1.2'}
                def finish(self):actions.append('close');return True
            args=SimpleNamespace(package=Path('/synthetic'),public_key=Path('/public'),trust_sha256='synthetic',saved_network=True,remember_network=False)
            def handoff(session,blob,decode,confirm,outcome):
                outcome['post_started']=posted
                raise TimeoutError('synthetic-password reflected token')
            with patch.object(sys.stdin,'isatty',return_value=True),patch.object(sys.stderr,'isatty',return_value=True),patch.object(stream,'isatty',return_value=True),contextlib.redirect_stdout(stream),patch('builtins.input',return_value='CONNECT'),patch.object(cli.artifacts,'preflight_package',return_value=b'synthetic'),patch.object(cli.maintenance,'BluezLink',return_value=object()),patch.object(cli.maintenance,'Session',return_value=Session()),patch.object(cli.wireless,'handoff',side_effect=handoff),patch.object(cli.signal,'alarm'),patch.object(cli.signal,'signal'):
                with self.assertRaises(TimeoutError):cli.terminal(args,{'device_address':'synthetic'})
            self.assertEqual(actions,['disconnect'] if posted else ['close'])
            self.assertNotIn('synthetic-password',stream.getvalue());self.assertNotIn('token',stream.getvalue())

class USBFailureTests(unittest.TestCase):
    def test_partial_inactive_app_failure_leaves_old_selector(self):
        plan,flash=setup()
        class Partial(FakeRom):
            def write(self,start,data):
                if start==self.plan.start:
                    self.flash[start:start+1024]=data[:1024];raise TimeoutError()
                super().write(start,data)
        rom=Partial(plan,flash);result=usb.execute(plan,rom)
        self.assertFalse(result['success']);self.assertTrue(result['recovery_confirmed'])
        self.assertEqual(rom.read(usb.OTADATA,8192),flash[usb.OTADATA:usb.OTADATA+8192])
    def test_bad_app_readback_never_selects_candidate(self):
        plan,flash=setup()
        class Corrupt(FakeRom):
            def write(self,start,data):
                super().write(start,data)
                if start==self.plan.start:self.flash[start+1000]^=1
        rom=Corrupt(plan,flash);result=usb.execute(plan,rom)
        self.assertFalse(result['success']);self.assertTrue(result['recovery_confirmed'])
        self.assertFalse(any(start>=usb.OTADATA for start,size in rom.writes))
    def test_protected_config_change_refuses_recovery(self):
        plan,flash=setup()
        class CorruptProtected(FakeRom):
            def write(self,start,data):
                super().write(start,data)
                if start==self.plan.start:self.flash[0x300000]^=1
        rom=CorruptProtected(plan,flash);result=usb.execute(plan,rom)
        self.assertFalse(result['success']);self.assertFalse(result['recovery_confirmed'])
        self.assertEqual(result['recovery_error_type'],'Rejected')
        self.assertFalse(any(start>=usb.OTADATA for start,size in rom.writes))
