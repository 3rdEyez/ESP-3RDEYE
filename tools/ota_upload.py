#!/usr/bin/env python3
"""Computer-to-device single-shot OTA upload. Default is offline package plan."""
import argparse,getpass,http.client,ipaddress,json,socket,threading,time,sys,warnings
from pathlib import Path
from ota_package import decode_package

def endpoint(value):
    address=ipaddress.IPv4Address(value)
    if not address.is_private or address.is_unspecified or address.is_multicast or address==ipaddress.IPv4Address('255.255.255.255'):
        raise ValueError('Use the device IPv4 displayed by the current maintenance window')
    return str(address)

def upload_once(host,package,token,ap=False,timeout=30,connection_factory=http.client.HTTPConnection):
    # Local checks before any connection. A SHA check does not authenticate RSA.
    image,manifest=decode_package(package);host=endpoint(host)
    if not ap and (len(token)!=32 or any(c not in '0123456789abcdef' for c in token)):
        raise ValueError('LAN requires the current 32-character window authorization code')
    if ap and token:raise ValueError('AP fallback does not use a LAN token')
    conn=connection_factory(host,80,timeout=timeout);deadline=time.monotonic()+timeout;expired=threading.Event()
    def abort():
        expired.set()
        if conn.sock:
            try:conn.sock.shutdown(socket.SHUT_RDWR)
            except OSError:pass
        conn.close()
    timer=threading.Timer(timeout,abort);timer.daemon=True;timer.start()
    try:
        conn.connect()
        if expired.is_set():raise TimeoutError('Upload deadline')
        conn.sock.settimeout(max(.001,deadline-time.monotonic()))
        headers={'Content-Length':str(len(image)),'Content-Type':'application/octet-stream',
                 'X-Satori-Manifest':json.dumps(manifest,separators=(',',':')),'X-Satori-Install':'confirm-restart'}
        if not ap:headers['X-Satori-Window']=token
        # Exactly one request; no redirects, retries, automatic close or BLE action.
        conn.request('POST','/v1/ota/image',body=image,headers=headers)
        if expired.is_set():raise TimeoutError('Upload deadline')
        conn.sock.settimeout(max(.001,deadline-time.monotonic()))
        response=conn.getresponse();status=response.status
        response.read(4096) # bounded, never display possibly reflected data
        if expired.is_set():raise TimeoutError('Upload deadline')
        return {'http_status':status,'submitted_for_restart':status==200,'upgrade_verified':False,
                'version':manifest['version'],'request_count':1}
    finally:
        timer.cancel();conn.close()

def hidden_token():
    if not sys.stdin.isatty() or not sys.stdout.isatty() or not sys.stderr.isatty():
        raise ValueError('A private interactive TTY is required for the window code')
    with warnings.catch_warnings():
        warnings.simplefilter('error',getpass.GetPassWarning)
        return getpass.getpass('Current LAN window authorization code (hidden): ')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('package',type=Path)
    parser.add_argument('--host',help='Device IPv4 from the maintenance page');parser.add_argument('--ap-fallback',action='store_true')
    parser.add_argument('--install-and-restart',action='store_true',help='Explicitly authorize one upload and device restart')
    args=parser.parse_args();blob=args.package.read_bytes();_,manifest=decode_package(blob)
    if not args.install_and_restart:
        print(json.dumps({'offline_plan':True,'metadata':manifest,'signature_verified':False,'network_access':False},indent=2));return 0
    if not args.host:parser.error('--host is required for installation')
    # Prompt at the terminal. Never accept secrets in argv or print exception data.
    token=''
    try:
        if not args.ap_fallback:token=hidden_token()
        result=upload_once(args.host,blob,token,args.ap_fallback);print(json.dumps(result,indent=2))
        if result['submitted_for_restart']:print('Submitted. Reconnect and verify the installed firmware version and VALID boot state; motion does not resume automatically.')
        else:print('Device rejected upload. Do not automatically resend.')
        return 0 if result['submitted_for_restart'] else 2
    except Exception as exc:
        print(json.dumps({'upload_result':'unknown_or_failed','error_type':type(exc).__name__,'automatic_retry':False}))
        return 2
    finally:token=''
if __name__=='__main__':raise SystemExit(main())
