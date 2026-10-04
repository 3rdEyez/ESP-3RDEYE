"""Explicit Linux ROM transport; no side effects when imported."""
import os,re,signal,stat,subprocess,time
from pathlib import Path
from .runtime import Rejected,digest as sha
FLASH=0x400000
OTADATA=0x303000

def passive_guard(port,identity):
    node=os.stat(port)
    if not stat.S_ISCHR(node.st_mode):raise Rejected('Serial node is not a character device')
    parents=(Path('/sys/class/tty')/Path(port).resolve().name/'device').resolve()
    usb=next((p for p in (parents,*parents.parents) if (p/'idVendor').is_file()),None)
    if usb is None or set(identity)!={'vid','pid','serial'}:raise Rejected('USB identity configuration required')
    for field,key in [('idVendor','vid'),('idProduct','pid'),('serial','serial')]:
        if (usb/field).read_text().strip()!=identity[key]:raise Rejected('USB identity mismatch')
    result=subprocess.run(['fuser',str(port)],capture_output=True,check=False)
    if result.returncode!=1 or result.stdout or result.stderr:raise Rejected('Serial occupied or occupancy check unavailable')

def observe(port,seconds):
    until=time.monotonic()+seconds;pending=b'';lines=[]
    while time.monotonic()<until:
        pending+=port.read(min(max(port.in_waiting,1),4096))
        pieces=pending.split(b'\n');pending=pieces.pop()[-8192:]
        for raw in pieces:
            line=re.sub(r'\x1b\[[0-9;]*m','',raw.decode('utf-8','replace')).strip()
            if re.search(r'ESP-ROM:|^rst:0x|Guru Meditation|panic(?:ked)?|Backtrace:|abort\(\)|assert failed|Stack protection fault|\bboot:|\bOTA_BOOT:|\bOTA_FAULT:|Boot reset_reason=|main_task: (Started|Calling app_main|Returned)',line) and not re.search(r'passkey|password|token|credential|pair.*code',line,re.I):
                lines.append(line)
    return lines

def write_region(esp, start, padded):
    if esp.IS_STUB or esp.FLASH_WRITE_SIZE != 0x400 or start % 4096 or len(padded) % 4096:
        raise RuntimeError('Unexpected ROM writer geometry')
    # Full sector-sized data makes erase/write and final FF padding explicit.
    blocks = esp.flash_begin(len(padded), start, begin_rom_encrypted=False)
    if blocks != len(padded)//esp.FLASH_WRITE_SIZE:
        raise RuntimeError('Unexpected ROM block count')
    for sequence in range(blocks):
        low = sequence * esp.FLASH_WRITE_SIZE
        esp.flash_block(padded[low:low+esp.FLASH_WRITE_SIZE], sequence)
        if sequence % 128 == 0 or sequence == blocks-1:
            print('WRITE 0x%x: %d/%d blocks' % (start, sequence+1, blocks), flush=True)
    # Official no-stub CLI does not call FLASH_END between files. Keep ROM.
    actual = esp.read_flash(start, len(padded))
    if actual != padded:
        raise RuntimeError('Complete sector readback mismatch at 0x%x' % start)
    return sha(actual)



class Rom:
    def __init__(self,port,usb_identity,flash_id):self.port_name=port;self.usb_identity=usb_identity;self.flash_id=flash_id;self.opened=False;self.in_rom=False;self.port=None;self.esp=None;self.deadline=time.monotonic()+900
    def recovery_bound(self):self.deadline=time.monotonic()+300
    def timeout(self,seconds):
        remaining=self.deadline-time.monotonic()
        if remaining<=0:raise TimeoutError('Total workflow deadline')
        signal.alarm(max(1,min(seconds,int(remaining))))
    def open(self):
        import serial,esptool,esptool.loader
        if esptool.__version__!='4.8.1':raise Rejected('Required ROM tool differs')
        esptool.loader.WRITE_BLOCK_ATTEMPTS=1;passive_guard(self.port_name,self.usb_identity)
        self.port=serial.Serial(port=None,baudrate=115200,timeout=.25,write_timeout=2,exclusive=True);self.port.port=self.port_name;self.port.dtr=False;self.port.rts=False
        self.port.open();self.opened=True;self.enter()
    def enter(self):
        if self.in_rom:return
        from esptool.reset import USBJTAGSerialReset
        from esptool.targets.esp32c3 import ESP32C3ROM
        self.timeout(20);USBJTAGSerialReset(self.port).reset();self.esp=ESP32C3ROM(port=self.port,baud=115200);self.esp.connect(mode='no_reset',attempts=1);self.in_rom=True
        sec=self.esp.get_security_info()
        if self.esp.IS_STUB or self.esp.sync_stub_detected or sec['flags']!=0 or sec['flash_crypt_cnt'].bit_count()%2:raise Rejected('Unexpected ROM/security state')
        self.esp.flash_spi_attach(0)
        if self.esp.flash_id()!=self.flash_id:raise Rejected('Unexpected flash ID')
        self.esp.flash_set_parameters(FLASH)
    def read(self,start,length):self.timeout(180 if length==FLASH else 30);return self.esp.read_flash(start,length)
    def write(self,start,blob):self.timeout(240 if start!=OTADATA else 30);write_region(self.esp,start,blob)
    def boot(self):
        from esptool.reset import HardReset
        self.timeout(40);HardReset(self.port,uses_usb=True).reset();self.in_rom=False
    def observe(self,seconds):self.timeout(seconds+10);return observe(self.port,seconds)
    def close(self):
        signal.alarm(0)
        if self.port:self.port.close()
