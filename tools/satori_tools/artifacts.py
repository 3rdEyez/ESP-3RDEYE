import tempfile
from pathlib import Path
from ota_package import decode_package, encode_package, image_metadata
from .runtime import Rejected, checked, digest, sdk_command


def trust(public_key, expected_digest):
    public_key = Path(public_key)
    if len(expected_digest) != 64 or digest(public_key.read_bytes()) != expected_digest:
        raise Rejected('Public trust digest mismatch')
    return public_key


def verify_image(image, public_key, expected_digest):
    public_key = trust(public_key, expected_digest)
    image_metadata(image)
    with tempfile.TemporaryDirectory(prefix='satori-verify-') as directory:
        path = Path(directory) / 'image.bin'
        path.write_bytes(image)
        path.chmod(0o600)
        checked(sdk_command('espsecure') + ['verify_signature', '--version', '2', '--keyfile', str(public_key), str(path)])
    return image_metadata(image)


def package_verified(image_path, public_key, expected_digest, output=None):
    image = Path(image_path).read_bytes()
    metadata = verify_image(image, public_key, expected_digest)
    blob, _ = encode_package(image, 'esp-idf-sbv2-rsa3072')
    if output:
        # No overwrite and no misleading unsigned container output.
        with Path(output).open('xb') as stream:
            stream.write(blob)
    return {'metadata': metadata, 'signature_verified': True, 'package_written': output is not None}


def preflight_package(path, public_key, expected_digest):
    blob = Path(path).read_bytes()
    image, _ = decode_package(blob)
    verify_image(image, public_key, expected_digest)
    return blob


def sign_package(image_path, public_key, expected_digest, output, execute=False):
    image = Path(image_path).read_bytes()
    metadata = image_metadata(image)
    trust(public_key, expected_digest)
    if Path(output).exists():
        raise Rejected('Output already exists')
    if not execute:
        return {'offline_plan': True, 'metadata': metadata, 'signing_performed': False, 'requires': 'existing key file via SATORI_SIGNING_KEY_FILE and --execute'}
    # Key material is read only by the SDK on explicit execution, not by us.
    import os
    key_file = os.environ.get('SATORI_SIGNING_KEY_FILE')
    if not key_file:
        raise Rejected('Existing signing key file must be configured')
    with tempfile.TemporaryDirectory(prefix='satori-sign-') as directory:
        signed = Path(directory) / 'signed.bin'
        checked(sdk_command('espsecure') + ['sign_data', '--version', '2', '--keyfile', key_file, '--output', str(signed), str(Path(image_path))])
        return package_verified(signed, public_key, expected_digest, output)
