import importlib.util
from pathlib import Path
import struct
import unittest

spec=importlib.util.spec_from_file_location('ota_package',Path(__file__).resolve().parents[1]/'tools/ota_package.py')
model=importlib.util.module_from_spec(spec);spec.loader.exec_module(model)

def fixture():
    data=bytearray(b'\xff'*512);data[0]=0xe9
    struct.pack_into('<H',data,12,5);struct.pack_into('<I',data,32,0xabcd5432)
    data[48:80]=b'0.2.5\0'+b'\0'*26;data[288:336]=model.BOARD_TAG
    return bytes(data)

class PackageTests(unittest.TestCase):
    def test_exact_container_roundtrip(self):
        blob,meta=model.encode_package(fixture(),'esp-idf-sbv2-rsa3072')
        self.assertEqual(model.decode_package(blob),(fixture(),meta))
        self.assertNotIn('signature_verified',meta) # never inferred from fields
    def test_wrong_board_chip_version_or_size(self):
        for address,value in [(0,0),(12,4),(288,0),(48,10)]:
            image=bytearray(fixture());image[address]=value
            with self.assertRaises(ValueError):model.image_metadata(image)
        with self.assertRaises(ValueError):model.image_metadata(b'\xff'*(model.MAX_IMAGE+1))
    def test_corruption_truncation_and_overlong_rejected(self):
        package,_=model.encode_package(fixture(),'esp-idf-sbv2-rsa3072')
        for bad in (package[:-1],package+b'\xff',b'\xff'*4,package[:5]):
            with self.assertRaises((ValueError,UnicodeError)):model.decode_package(bad)
        changed=bytearray(package);changed[-1]^=1
        with self.assertRaises(ValueError):model.decode_package(changed)

if __name__=='__main__':unittest.main()
