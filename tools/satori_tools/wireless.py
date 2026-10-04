"""Single local GET then authorized POST; no bearer export or retry."""
import http.client,socket,threading
import ota_package,ota_upload
from .runtime import Rejected
MIN_REMAINING_MS=45000
def probe(host,connection_factory=http.client.HTTPConnection):
    uploader=ota_upload;host=uploader.endpoint(host);conn=connection_factory(host,80,timeout=5)
    expired=threading.Event()
    def abort():
        expired.set()
        if conn.sock:
            try:conn.sock.shutdown(socket.SHUT_RDWR)
            except OSError:pass
        conn.close()
    timer=threading.Timer(5,abort);timer.daemon=True;timer.start()
    try:
        conn.request('GET','/',headers={'Connection':'close'})
        response=conn.getresponse();body=response.read(4096)
        if expired.is_set() or response.status!=200 or '<title>觉瞳固件升级</title>'.encode() not in body:
            raise Rejected('设备升级页面检查未通过。')
    finally:timer.cancel();conn.close()

def handoff(session,blob,decode_status,confirm,outcome,probe_page=probe,upload=None):
    package,uploader=ota_package,ota_upload;upload=upload or uploader.upload_once
    _,manifest=package.decode_package(blob);offered_version=manifest['version']
    def current():
        s=decode_status(session.link.read())
        if s['state']!=2 or s['result']!=0 or s['ack']!=session.rid or s['window']!=session.window or not s['window']:
            raise Rejected('当前维护窗口不匹配或未就绪。')
        if s['remaining_ms']<MIN_REMAINING_MS:raise Rejected('剩余窗口不足45秒，未上传；请结束后另行重开。')
        uploader.endpoint(s['ip'])
        return s
    initial=current();probe_page(initial['ip'])
    # The user confirms locally while the original terminal timer keeps ticking.
    if confirm('页面已确认。安装'+offered_version+'并重启会写入非活动槽，保留当前VALID槽用于回滚。亲自输入 INSTALL 确认；其他输入取消：')!='INSTALL':
        raise Rejected('已取消安装，未发送POST。')
    s=current()
    if s['ip']!=initial['ip'] or s['token']!=initial['token']:raise Rejected('窗口地址或授权已变化，未上传。')
    token=s['token'].decode('ascii')
    try:
        result=upload(s['ip'],blob,token,timeout=30,on_post_start=lambda:outcome.update(post_started=True))
        outcome['submitted']=result['submitted_for_restart']
        return result
    finally:token='';initial=None;s=None
