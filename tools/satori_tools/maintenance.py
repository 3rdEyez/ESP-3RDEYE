"""Existing-bond maintenance, configured device; no scanning or control commands."""
import struct,time,threading,secrets
from .runtime import Rejected
UUID='4d89f6a0-73b9-4f14-9d3e-63b2145a0008'
def wipe(buffer):
    for i in range(len(buffer)):buffer[i]=0

def request(password,rid,ssid):
    if not 1<=len(ssid)<=32 or any(v<32 or v==127 for v in ssid):raise Rejected('Invalid configured SSID')
    if not 8<=len(password)<=63 or any(v<32 or v>126 for v in password):raise Rejected('密码须为8–63个ASCII字符。')
    return bytearray(struct.pack('<BBIIBB',1,1,rid,0,len(ssid),len(password))+ssid+password)

def status(raw):
    if len(raw)<24 or raw[0]!=1 or raw[1]>6 or raw[2]>6 or raw[3]>6 or raw[20] not in (0,32) or len(raw)!=24+raw[20] or any(raw[21:24]):raise Rejected('设备状态格式不符。')
    state=raw[1];ip=raw[16:20];token=bytes(raw[24:])
    if state in (2,3):
        if not int.from_bytes(raw[8:12],'little') or not raw[20] or not 0<ip[0]<224 or all(v==255 for v in ip) or any(c not in b'0123456789abcdef' for c in token):raise Rejected('设备尚未确认可用地址。')
    elif token:raise Rejected('设备状态格式不符。')
    return {'state':state,'result':raw[2],'detail':raw[3],'ack':int.from_bytes(raw[4:8],'little'),'window':int.from_bytes(raw[8:12],'little'),'remaining_ms':int.from_bytes(raw[12:16],'little'),'ip':'.'.join(map(str,ip)),'token':token}

class Session:
    def __init__(self,link,ssid=b'',rid=None,clock=time.monotonic,pause=time.sleep):
        self.link=link;self.ssid=ssid;self.rid=rid or secrets.randbelow(0xfffffffd)+1;self.clock=clock;self.pause=pause;self.window=0;self.sent=False;self.connected=False;self.ready=None
    def close(self):
        if not self.sent:return True
        deadline=self.clock()+8
        # Reconcile a possibly accepted write before trying CLOSE. Never close an unrelated window.
        s=status(self.link.read())
        if s['state']==0:return True
        if s['ack']!=self.rid or not s['window']:return False
        wid=s['window'];rid=self.rid+1
        self.link.write(bytearray(struct.pack('<BBIIBB',1,2,rid,wid,0,0)))
        while self.clock()<deadline:
            s=status(self.link.read())
            if s['ack']==rid and s['state']==0 and s['result']==0:return True
            self.pause(.2)
        return False
    def open(self,password,cancel,remember_network=False,use_saved_network=False):
        command=None
        try:
            if use_saved_network:
                if remember_network or password:raise Rejected('复用网络不接受新密码或同时保存。')
                command=bytearray(struct.pack('<BBIIBB',2,1,self.rid,0,0,0))
            else:
                command=request(password,self.rid,self.ssid)
                if remember_network:command[0]=3 # explicit consent only
            if cancel.is_set():raise Rejected('已取消，未发送配网。')
            self.link.connect();self.connected=True
            version=self.link.version()
            if not version.startswith('0.2.'):raise Rejected('固件须与App处于0.2.x兼容线。')
            if remember_network or use_saved_network:
                if not self.link.saved_feature(use_saved_network):raise Rejected('设备不支持该选项或没有已记住网络。')
            initial=status(self.link.read())
            if initial['state']!=0 or initial['result']!=0:raise Rejected('设备已有窗口或签名维护未就绪。')
            if cancel.is_set():raise Rejected('已取消，未发送配网。')
            self.sent=True;self.link.write(command) # one atomic OPEN, never blind retries
            wipe(command);wipe(password)
            deadline=self.clock()+26
            while self.clock()<deadline:
                if cancel.is_set():raise Rejected('正在取消维护。')
                s=status(self.link.read())
                if s['ack']==self.rid:
                    self.window=s['window']
                    if s['result'] or s['state']==6:
                        if s['detail']==6:raise Rejected('保存结果未确认，维护已关闭；设备可能已记住新网络，请读取状态或重新配网。')
                        if s['detail']==5:raise Rejected('没有可用的已记住网络，请重新配网。')
                        raise Rejected('连接失败，请检查2.4GHz网络和密码。')
                    if s['state']==2 and s['remaining_ms']>0:self.ready=s;return s
                    if s['state']==0:raise Rejected('窗口已关闭或到期。')
                self.pause(.2)
            raise Rejected('连接未确认，正在清理本次窗口。')
        except BaseException as error:
            closed=False
            if self.sent:
                try:closed=self.close()
                except BaseException:pass
            if self.connected or getattr(self.link,'owned',False):
                try:self.link.disconnect()
                except BaseException:pass
                self.connected=False
            # No transport/exception representation: it may contain user input.
            message=str(error) if isinstance(error,Rejected) else '连接或状态读取失败。'
            if self.sent and not closed:message+=' 关闭未确认，请等待设备窗口到期（最长十分钟）；不要自动重试。'
            raise Rejected(message) from None
        finally:
            if command is not None:wipe(command)
            wipe(password)
    def finish(self):
        try:return self.close()
        finally:
            self.ready=None
            if self.connected:
                try:self.link.disconnect()
                finally:self.connected=False

class BluezLink:
    owned=False
    def __init__(self,address,adapter='hci0'):
        import re
        if not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}',address):raise Rejected('Invalid configured BLE address')
        if not re.fullmatch(r'hci[0-9]+',adapter):raise Rejected('Invalid configured BLE adapter')
        self.device='/org/bluez/'+adapter+'/dev_'+address.upper().replace(':','_')
    def connect(self):
        import dbus
        self.dbus=dbus;self.bus=dbus.SystemBus();obj=self.bus.get_object('org.bluez',self.device)
        props=dbus.Interface(obj,'org.freedesktop.DBus.Properties');self.dev=dbus.Interface(obj,'org.bluez.Device1')
        if not bool(props.Get('org.bluez.Device1','Paired',timeout=3)):raise Rejected('需要设备已有的BLE绑定，不会创建新绑定。')
        if bool(props.Get('org.bluez.Device1','Connected',timeout=3)):raise Rejected('设备BLE正被占用，请先停止现有控制连接。')
        self.owned=True
        self.dev.Connect(timeout=12)
        deadline=time.monotonic()+12
        while not bool(props.Get('org.bluez.Device1','ServicesResolved',timeout=3)):
            if time.monotonic()>deadline:raise Rejected('BLE服务解析超时。')
            time.sleep(.2)
        manager=dbus.Interface(self.bus.get_object('org.bluez','/'),'org.freedesktop.DBus.ObjectManager');self.paths={}
        for p,ifs in manager.GetManagedObjects(timeout=5).items():
            c=ifs.get('org.bluez.GattCharacteristic1')
            if str(p).startswith(self.device+'/') and c:self.paths[str(c.get('UUID',''))]=str(p)
        if UUID not in self.paths:raise Rejected('设备不支持LAN维护。')
    def characteristic(self,uuid):return self.dbus.Interface(self.bus.get_object('org.bluez',self.paths[uuid]),'org.bluez.GattCharacteristic1')
    def version(self):
        b=bytes(self.characteristic('4d89f6a0-73b9-4f14-9d3e-63b2145a0002').ReadValue({},timeout=3))
        if len(b)!=20 or b[0]!=1 or b[1]!=2 or (int.from_bytes(b[6:10],'little')&0x5f)!=0x5f:raise Rejected('设备版本读取失败。')
        return '.'.join(map(str,b[2:5]))
    def saved_feature(self,require_saved):
        uuid='4d89f6a0-73b9-4f14-9d3e-63b2145a0009'
        if uuid not in self.paths:return False
        b=bytes(self.characteristic(uuid).ReadValue({},timeout=3))
        return len(b)==4 and b[0]==1 and b[1]==3 and b[2]<=1 and b[3]==0 and (not require_saved or b[2]==1)
    def read(self):return bytes(self.characteristic(UUID).ReadValue({},timeout=3))
    def write(self,command):self.characteristic(UUID).WriteValue(self.dbus.Array(command,signature='y'),{'type':'request'},timeout=5)
    def disconnect(self):
        if self.owned:self.dev.Disconnect(timeout=5);self.owned=False
