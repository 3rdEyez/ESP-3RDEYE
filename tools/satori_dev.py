#!/usr/bin/env python3
"""Satori local build/check/artifact/maintenance/USB entry. Offline by default."""
import argparse
import json
import os
from pathlib import Path
import signal
import sys
import threading

from satori_tools import artifacts, checks, maintenance, usb_upgrade, wireless, migration, ota_records
from satori_tools.runtime import ROOT, Rejected, LayoutRejected, digest, private_output


class Parser(argparse.ArgumentParser):
    def error(self, message):
        # Do not echo unknown argv values: someone may have supplied a secret.
        self.print_usage(sys.stderr)
        self.exit(2, 'Invalid arguments; see --help. Do not put secrets in argv.\n')


def parser():
    result = Parser(description=__doc__)
    commands = result.add_subparsers(dest='command', required=True, parser_class=Parser)
    check = commands.add_parser('check', help='run synthetic firmware/optional App regressions')
    check.add_argument('--app-path', type=Path)
    check.add_argument('--flutter', help='Flutter executable; defaults to PATH')
    build = commands.add_parser('build', help='default unsigned dual OTA; historical factory profiles explicit; never flash')
    build.add_argument('profile', nargs='?', default='ble_wifi_ota_prototype', choices=('ble_primary', 'legacy_udp', 'ble_dual_ota', 'ble_wifi_ota_prototype'), help='default: ble_wifi_ota_prototype (dual OTA)')
    inspect = commands.add_parser('inspect', help='offline image or .sota metadata; no authentication claim')
    inspect.add_argument('path', type=Path)
    inspect.add_argument('--package', action='store_true')
    for name in ('package', 'sign-package'):
        command = commands.add_parser(name, help='verify/package, or plan SDK signing before packaging')
        command.add_argument('--image', type=Path, required=True)
        command.add_argument('--public-key', type=Path, required=True)
        command.add_argument('--trust-sha256', required=True, help='explicit existing public-key fingerprint')
        command.add_argument('--output', type=Path, required=True)
        command.add_argument('--execute', action='store_true', help='explicitly write output; sign-package also invokes existing key')
    for name in ('maintenance', 'status'):
        command = commands.add_parser(name, help='offline plan; --execute uses existing bonded BlueZ device')
        command.add_argument('--config', type=Path, required=True, help='private JSON: device_address, optional ssid; no password')
        command.add_argument('--execute', action='store_true', help='explicit BLE connect/read; maintenance also prompts personal CONNECT')
        if name == 'maintenance':
            mode = command.add_mutually_exclusive_group()
            mode.add_argument('--remember-network', action='store_true')
            mode.add_argument('--saved-network', action='store_true')
            command.add_argument('--package', type=Path, help='verified single upload after personal INSTALL; bearer stays internal')
            command.add_argument('--public-key', type=Path)
            command.add_argument('--trust-sha256')
    layout = commands.add_parser('check-layout', help='offline exact partition table check; no device access')
    layout.add_argument('--table', type=Path, required=True)
    migrate = commands.add_parser('migrate-layout', help='explicit factory-to-dual plan; never automatic')
    migrate.add_argument('--config', type=Path, required=True)
    migrate.add_argument('--backup-dir', type=Path)
    migrate.add_argument('--execute', action='store_true', help='requires private TTY and personally typed MIGRATE; resets/writes bootloader and partition table')
    usb = commands.add_parser('usb-upgrade', help='normal app-only plan; --execute resets and writes the device')
    usb.add_argument('--config', type=Path, required=True, help='private artifact/reference/USB identity config; see README')
    usb.add_argument('--backup-dir', type=Path, help='new private directory outside checkout, mandatory for execution')
    usb.add_argument('--execute', action='store_true', help='explicit resets, double backup, inactive app and selector writes')
    rollback = commands.add_parser('rollback-plan', help='selector model only; never fault injection')
    rollback.add_argument('--current-slot', type=int, choices=(0, 1), required=True)
    rollback.add_argument('--current-sequence', type=int, required=True)
    return result


def configuration(path):
    config = json.loads(path.read_text())
    def contains_secret(value):
        if isinstance(value, dict):
            return any(any(word in str(key).lower() for word in ('password', 'token', 'private_key')) or contains_secret(item) for key, item in value.items())
        return isinstance(value, list) and any(contains_secret(item) for item in value)
    if not isinstance(config, dict) or contains_secret(config):
        raise Rejected('Configuration must not contain secrets')
    return config


def public_status(value):
    return {key: value[key] for key in ('state', 'result', 'detail', 'remaining_ms', 'ip')}


def terminal(args, config):
    import getpass
    import warnings
    if not all(stream.isatty() for stream in (sys.stdin, sys.stdout, sys.stderr)):
        raise Rejected('Use a private interactive TTY, never pipes or redirects')
    blob = None
    if args.package:
        if not args.public_key or not args.trust_sha256:
            raise Rejected('Existing public key and trust fingerprint required')
        blob = artifacts.preflight_package(args.package, args.public_key, args.trust_sha256)
    ssid = config.get('ssid', '').encode('utf-8')
    if not args.saved_network and (not 1 <= len(ssid) <= 32 or any(v < 32 or v == 127 for v in ssid)):
        raise Rejected('Configure a valid SSID before personal submission')
    password = bytearray()
    session = None
    outcome = {'post_started': False}
    previous_alarm = signal.getsignal(signal.SIGALRM)
    try:
        if not args.saved_network:
            with warnings.catch_warnings():
                warnings.simplefilter('error', getpass.GetPassWarning)
                raw = getpass.getpass('Wi-Fi password (hidden, never logged): ')
            try:
                password = bytearray(raw, 'ascii')
                probe = maintenance.request(password, 1, ssid)
                maintenance.wipe(probe)
            finally:
                raw = ''
        if args.remember_network:
            print('Remember selected: store only on the device after confirmed IP; no automatic connection.')
        if input('Personally type CONNECT to open maintenance and pause motion; anything else cancels: ') != 'CONNECT':
            return {'cancelled': True, 'device_accessed': False}
        session = maintenance.Session(maintenance.BluezLink(config['device_address'], config.get('adapter', 'hci0')), ssid=ssid)
        value = session.open(password, threading.Event(), args.remember_network, args.saved_network)
        print(json.dumps(public_status(value)))
        def expired(*_):
            raise Rejected('Maintenance deadline reached')
        signal.signal(signal.SIGALRM, expired)
        signal.alarm(max(1, value['remaining_ms'] // 1000))
        if blob:
            return wireless.handoff(session, blob, maintenance.status, input, outcome)
        input('Connectivity confirmed. Press Enter to close this window: ')
        return {'window_checked': True, 'upload_sent': False}
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, previous_alarm)
        maintenance.wipe(password)
        if session and session.connected:
            if outcome['post_started']:
                # Lost ACK can mean committed/restarting: do not CLOSE or retry.
                session.ready = None
                try:
                    session.link.disconnect()
                except BaseException:
                    pass
                print('POST was attempted once. Verify installed version/state before further action.')
            else:
                try:
                    closed = session.finish()
                except BaseException:
                    closed = False
                print('Window closure confirmed.' if closed else 'Closure unknown; wait for expiry, no automatic retry.')


def status_once(config):
    link = maintenance.BluezLink(config['device_address'], config.get('adapter', 'hci0'))
    try:
        link.connect()
        value = maintenance.status(link.read())
        return {'firmware': link.version(), **public_status(value), 'token_present': bool(value['token']), 'writes_sent': 0}
    finally:
        if link.owned:
            link.disconnect()


def usb_plan(config, base):
    def load(name):
        item = config[name]
        path = (base / item['path']).resolve()
        blob = path.read_bytes()
        if digest(blob) != item['sha256']:
            raise Rejected('Configured artifact fingerprint mismatch')
        return path, blob
    image_path, image = load('candidate')
    current_path, current = load('current_app')
    _, boot = load('bootloader')
    _, table = load('partition_table')
    ota_records.require_dual(table)
    public = (base / config['public_key']).resolve()
    artifacts.verify_image(image, public, config['trust_sha256'])
    artifacts.verify_image(current, public, config['trust_sha256'])
    return usb_upgrade.Plan(image, current, boot, table, config['candidate']['sha256'], lambda: True,
                            config['current_slot'], 1 - config['current_slot'], config['current_app']['sha256'])


def migration_plan(config, base):
    def load(name):
        item = config[name]; value = (base / item['path']).read_bytes()
        if digest(value) != item['sha256']: raise Rejected('Migration artifact identity differs')
        return value
    image = load('candidate')
    public = (base / config['public_key']).resolve()
    old_table = load('old_partition_table')
    if ota_records.classify_table(old_table) != 'factory':
        raise LayoutRejected('Migration requires supported factory layout; dual-ota devices update normally and unknown layouts require investigation')
    artifacts.verify_image(image, public, config['trust_sha256'])
    return migration.Plan(image, load('old_app'), load('old_bootloader'), old_table,
                          load('new_bootloader'), load('new_partition_table'), load('new_unsigned_app'),
                          json.loads(load('new_build_proof')), lambda: True)


def execute_usb(plan, args, config, executor=usb_upgrade.execute):
    from satori_tools.usb_transport import Rom
    if not args.backup_dir:
        raise Rejected('Explicit private backup directory required')
    rom = Rom(config['port'], config['usb_identity'], int(config['flash_id'], 0))
    output = private_output(args.backup_dir)
    # No directory creation before all offline inputs have passed.
    output.mkdir(mode=0o700, parents=False, exist_ok=False)
    old_mask = os.umask(0o077)
    previous_alarm = signal.getsignal(signal.SIGALRM)
    def deadline(*_):
        raise TimeoutError()
    signal.signal(signal.SIGALRM, deadline)
    def sync_directory(path):
        descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        try: os.fsync(descriptor)
        finally: os.close(descriptor)
    def save(value):
        path = output / 'metadata.json'
        temporary = output / '.metadata.tmp'
        with temporary.open('w') as stream:
            json.dump(value, stream, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.chmod(0o600)
        os.replace(temporary, path)
        sync_directory(output)
    def backup(number, blob):
        with (output / ('read-%d.bin' % number)).open('xb') as stream:
            stream.write(blob)
            stream.flush()
            os.fsync(stream.fileno())
        sync_directory(output)
    try:
        sync_directory(output.parent)
        result = executor(plan, rom, save, backup)
        return {key: result.get(key) for key in ('success', 'phase', 'error_type', 'interrupted', 'recovery_confirmed', 'port_closed', 'left_in_rom', 'manual_recovery_required', 'boot_regions_write_started', 'automatic_recovery_attempted', 'guidance')}
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, previous_alarm)
        os.umask(old_mask)


def run(args):
    if args.command == 'check':
        return checks.run(args.app_path, args.flutter)
    if args.command == 'build':
        import build_firmware
        return {'build_exit_code': build_firmware.main_profile(args.profile)}
    if args.command == 'inspect':
        if args.package:
            _, metadata = artifacts.decode_package(args.path.read_bytes())
        else:
            metadata = artifacts.image_metadata(args.path.read_bytes())
        return {'metadata': metadata, 'signature_verified': False}
    if args.command == 'package':
        return artifacts.package_verified(args.image, args.public_key, args.trust_sha256, args.output if args.execute else None)
    if args.command == 'sign-package':
        return artifacts.sign_package(args.image, args.public_key, args.trust_sha256, args.output, args.execute)
    if args.command in ('maintenance', 'status'):
        config = configuration(args.config)
        if set(config) - {'device_address', 'ssid', 'adapter'}:
            raise Rejected('Unsupported maintenance configuration')
        if not args.execute:
            return {'offline_plan': True, 'device_accessed': False, 'requires': '--execute; maintenance also personal CONNECT and INSTALL for upload'}
        return terminal(args, config) if args.command == 'maintenance' else status_once(config)
    if args.command == 'check-layout':
        layout = ota_records.classify_table(args.table.read_bytes())
        return {'layout': layout, 'update_allowed': layout == 'dual-ota', 'success': layout == 'dual-ota',
                'guidance': 'Run explicit tools/migrate_layout.py for supported factory; unknown layout requires investigation; no automatic migration'}
    if args.command == 'migrate-layout':
        config = configuration(args.config)
        plan = migration_plan(config, args.config.resolve().parent)
        if not args.execute: return plan.public()
        if not all(stream.isatty() for stream in (sys.stdin, sys.stdout, sys.stderr)):
            raise Rejected('Migration requires private interactive TTY')
        if input('Migration writes bootloader/table; partial writes are not protected by app rollback. Personally type MIGRATE: ') != 'MIGRATE':
            return {'cancelled': True, 'device_accessed': False}
        return execute_usb(plan, args, config, migration.execute)
    if args.command == 'usb-upgrade':
        config = configuration(args.config)
        plan = usb_plan(config, args.config.resolve().parent)
        return execute_usb(plan, args, config) if args.execute else {'offline_plan': True, 'serial_opened': False, **plan.public()}
    if args.command == 'rollback-plan':
        import plan_ota_rollback_test
        return plan_ota_rollback_test.plan(args.current_slot, args.current_sequence, 4096)
    raise Rejected('Unsupported operation')


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        value = run(args)
        print(json.dumps(value, ensure_ascii=False, indent=2))
        return 0 if value.get('success', True) and value.get('build_exit_code', 0) == 0 else 2
    except (Exception, KeyboardInterrupt) as error:
        print(json.dumps({'operation_failed': True, 'error_type': type(error).__name__, 'details_withheld': True, 'automatic_retry': False, **({'guidance': str(error)} if isinstance(error, (LayoutRejected, wireless.ota_upload.MigrationRequired)) else {})}))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
