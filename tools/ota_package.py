#!/usr/bin/env python3
"""Offline package/metadata preparation, never signs or authenticates an image."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

MAX_IMAGE=0x170000
BOARD_TAG=b''.join(value.ljust(16,b'\0') for value in (b'SATORI_BOARD_V1',b'satori_c3_v1',b'esp32c3'))
assert len(BOARD_TAG)==48


def image_metadata(image):
    if len(image)<336 or len(image)>MAX_IMAGE or image[0]!=0xe9:
        raise ValueError('Invalid image header or size')
    if struct.unpack_from('<H',image,12)[0]!=5:
        raise ValueError('Only ESP32-C3 images are accepted')
    if image[288:336]!=BOARD_TAG:
        raise ValueError('Missing/mismatched satori_c3_v1 image board descriptor')
    if struct.unpack_from('<I',image,32)[0]!=0xabcd5432:
        raise ValueError('Missing ESP-IDF app descriptor')
    raw=image[48:80].split(b'\0')[0]
    version=raw.decode('ascii')
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)',version) or any(int(v)>65535 for v in version.split('.')):
        raise ValueError('Invalid semantic version')
    return {'schema':1,'board':'satori_c3_v1','chip':'esp32c3',
            'image_length':len(image),'sha256':hashlib.sha256(image).hexdigest(),
            'version':version}


def encode_package(image,image_format):
    # image_format is a declaration only; SHA/header precheck is NOT a signature
    # verification. Device acceptance policy remains separate and fail-closed.
    if image_format!='esp-idf-sbv2-rsa3072':
        raise ValueError('Only the proposed native format is prepared in this prototype')
    metadata=image_metadata(image);metadata['image_format']=image_format
    header=json.dumps(metadata,separators=(',',':'),ensure_ascii=True).encode('ascii')
    if len(header)>1024:raise ValueError('Metadata too large')
    return struct.pack('<I',len(header))+header+image,metadata


def decode_package(package):
    if len(package)<5 or len(package)>MAX_IMAGE+1028:raise ValueError('Invalid package size')
    length=struct.unpack_from('<I',package)[0]
    if not 1<=length<=1024 or len(package)<=4+length:raise ValueError('Invalid metadata length')
    def unique(pairs):
        result={}
        for key,value in pairs:
            if key in result:raise ValueError('Duplicate metadata key')
            result[key]=value
        return result
    declared=json.loads(package[4:4+length].decode('ascii'),object_pairs_hook=unique)
    if not isinstance(declared,dict) or type(declared.get('schema')) is not int or type(declared.get('image_length')) is not int:
        raise ValueError('Metadata requires integer schema and length')
    image=package[4+length:]
    expected=image_metadata(image);expected['image_format']='esp-idf-sbv2-rsa3072'
    if declared!=expected:raise ValueError('Metadata differs from actual image')
    return image,expected


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image',type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    blob,metadata=encode_package(args.image.read_bytes(),'esp-idf-sbv2-rsa3072')
    print(json.dumps({'metadata':metadata,'signature_verified':False,'authenticity_established':False,
                      'device_transfer_allowed_by_this_tool':False},indent=2))
    if args.output:
        # Reject unsigned prototype images before producing a deployable file.
        # This structural check is still not signature authentication.
        import binascii
        block=args.image.read_bytes()[-4096:-4096+1216]
        if len(block)!=1216 or block[:2]!=b'\xe7\x02' or struct.unpack_from('<I',block,1196)[0]!=(binascii.crc32(block[:1196])&0xffffffff):
            raise ValueError('No structurally valid first native signature block; no output written')
        # Prototype declaration does not establish signature authenticity. Never
        # overwrite artifacts or claim that this offline container is trusted.
        with args.output.open('xb') as stream:stream.write(blob)


if __name__=='__main__':main()
