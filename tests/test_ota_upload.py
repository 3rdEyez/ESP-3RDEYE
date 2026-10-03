import contextlib,io,json,struct,sys,unittest,socket,threading,time,http.client
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import ota_package,ota_upload
class Sock:
 def settimeout(self,t):pass
 def shutdown(self,*a):pass
class Connection:
 instances=[];fail=False
 def __init__(self,*a,**kw):self.sock=Sock();self.requests=[];self.closed=False;self.__class__.instances.append(self)
 def connect(self):pass
 def request(self,*a,**kw):
  self.requests.append((a,kw))
  if self.fail:raise TimeoutError('sensitive-secret')
 def getresponse(self):return self
 status=200
 def read(self,n):return b'sensitive-server-response'
 def close(self):self.closed=True
class Tests(unittest.TestCase):
 def package(self):
  image=bytearray(336);image[0]=0xe9;struct.pack_into('<H',image,12,5);struct.pack_into('<I',image,32,0xabcd5432);image[48:53]=b'0.2.6';image[288:336]=ota_package.BOARD_TAG
  return ota_package.encode_package(bytes(image),'esp-idf-sbv2-rsa3072')[0]
 def setUp(self):Connection.instances=[];Connection.fail=False
 def test_one_post_no_secret_result(self):
  token='a'*32;r=ota_upload.upload_once('192.168.1.80',self.package(),token,connection_factory=Connection)
  self.assertEqual(len(Connection.instances[0].requests),1);self.assertTrue(r['submitted_for_restart']);self.assertFalse(r['upgrade_verified']);self.assertNotIn(token,json.dumps(r));self.assertTrue(Connection.instances[0].closed)
 def test_timeout_does_not_retry(self):
  Connection.fail=True
  with self.assertRaises(TimeoutError):ota_upload.upload_once('192.168.1.80',self.package(),'a'*32,connection_factory=Connection)
  self.assertEqual(len(Connection.instances[0].requests),1);self.assertTrue(Connection.instances[0].closed)
 def test_invalid_input_no_connect(self):
  for host,token in [('8.8.8.8','a'*32),('192.168.1.80','secret'),('0.0.0.0','a'*32)]:
   with self.assertRaises(ValueError):ota_upload.upload_once(host,self.package(),token,connection_factory=Connection)
  self.assertEqual(Connection.instances,[])
 def test_package_preflight_no_connect(self):
  with self.assertRaises(ValueError):ota_upload.upload_once('192.168.1.80',b'bad','a'*32,connection_factory=Connection)
  self.assertEqual(Connection.instances,[])
 def test_real_loopback_stalled_response_has_total_deadline(self):
  listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);port=listener.getsockname()[1];release=threading.Event();accepted=[]
  def server():
   peer,_=listener.accept();accepted.append(True)
   try:peer.recv(4096);release.wait(2)
   finally:peer.close()
  thread=threading.Thread(target=server,daemon=True);thread.start();started=time.monotonic()
  try:
   with self.assertRaises((TimeoutError,OSError,http.client.HTTPException)):
    ota_upload.upload_once('127.0.0.1',self.package(),'a'*32,timeout=.2,connection_factory=lambda host,unused,timeout:http.client.HTTPConnection(host,port,timeout=timeout))
   self.assertLess(time.monotonic()-started,1);self.assertEqual(accepted,[True])
  finally:release.set();listener.close();thread.join(timeout=1)
 def test_ap_explicit(self):
  r=ota_upload.upload_once('192.168.4.1',self.package(),'',True,connection_factory=Connection)
  self.assertTrue(r['submitted_for_restart']);self.assertNotIn('X-Satori-Window',Connection.instances[0].requests[0][1]['headers'])
if __name__=='__main__':unittest.main()
