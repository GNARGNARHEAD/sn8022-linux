#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Install, verify or remove this DKMS release for the next normal boot."""
import argparse
from contextlib import ExitStack
import ctypes
from datetime import datetime, timezone
import fcntl
import json
import hashlib
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess

import check

VERSION = check.INFO['version']
NAME = check.INFO['name']
SOURCE = Path('/usr/src') / (NAME + '-' + VERSION)
TREE = Path('/var/lib/dkms')
STATE = Path('/var/lib/sn8022-portable')
LIVE = Path('/sys/module/snd_hda_codec_conexant/srcversion')


def lock_package(fd):
    class Flock(ctypes.Structure):
        _fields_ = [('type', ctypes.c_short), ('whence', ctypes.c_short),
                    ('start', ctypes.c_longlong), ('length', ctypes.c_longlong), ('pid', ctypes.c_int)]
    check.require(platform.machine() == 'x86_64' and ctypes.sizeof(Flock) == 32, 'Unsupported lock ABI')
    fcntl.fcntl(fd, fcntl.F_OFD_SETLK, bytes(Flock(fcntl.F_WRLCK, os.SEEK_SET, 0, 0, 0)))


def selected(kernel):
    p = Path(check.run(['modinfo', '-k', kernel, '-F', 'filename', check.MODULE], text=True).stdout.strip())
    check.require(p.is_file() and p.resolve().is_relative_to((Path('/lib/modules') / kernel).resolve()),
                  'Selected module is outside the target kernel tree')
    return p


def dkms_status(kernel):
    return check.run(['dkms', 'status', '-m', NAME, '-k', kernel], text=True).stdout


def registered_source():
    link = TREE / NAME / VERSION / 'source'
    check.require(link.is_symlink() and link.resolve() == SOURCE.resolve(),
                  'DKMS registration points to a different source package')
    check.root_source(SOURCE)


def reject_unmanaged_replacement(kernel):
    p = selected(kernel)
    if '/kernel/' in str(p):
        return
    for line in dkms_status(kernel).splitlines():
        match = re.fullmatch(r'sn8022/([0-9][A-Za-z0-9.+-]*), ' + re.escape(kernel) +
                             r', x86_64: installed(?: \(.*\))?', line)
        if not match:
            continue
        version = match[1]
        source = Path('/usr/src') / ('sn8022-' + version)
        info = source / 'release.json'
        if info.is_file() and json.loads(info.read_text()).get('package_format') == 1:
            check.root_source(source)
            link = TREE / NAME / version / 'source'
            check.require(link.is_symlink() and link.resolve() == source.resolve(), 'Other release registration differs')
            built = TREE / NAME / version / kernel / 'x86_64/module' / p.name
            if built.is_file() and check.module_bytes(built) == check.module_bytes(p):
                return
    raise RuntimeError('Another/local Conexant replacement is installed; migrate it explicitly before using this package')


class Transaction:
    def __init__(self, kernel, action, fds):
        STATE.mkdir(mode=0o755, exist_ok=True)
        check.require(not STATE.is_symlink() and STATE.stat().st_uid == 0 and
                      not stat.S_IMODE(STATE.stat().st_mode) & 0o022, 'Unexpected transaction directory ownership')
        self.folder = STATE / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
        self.folder.mkdir(mode=0o700)
        self.kernel, self.fds = kernel, fds
        self.record = {'version': VERSION, 'kernel': kernel, 'action': action, 'status': 'started',
                       'commands': [], 'live_before': LIVE.read_text().strip() if LIVE.exists() else None}
        image = Path('/boot') / ('initrd.img-' + kernel)
        check.require(image.is_file(), 'Expected an existing initramfs')
        check.require(shutil.disk_usage(STATE).free > 4 * image.stat().st_size + 512 * 1024 * 1024,
                      'Insufficient space for backup and image verification')
        shutil.copyfile(image, self.folder / 'initrd.before')
        check.require(check.sha(image) == check.sha(self.folder / 'initrd.before'), 'Boot backup differs')
        old = selected(kernel)
        shutil.copyfile(old, self.folder / old.name)
        check.require(check.sha(old) == check.sha(self.folder / old.name), 'Module backup differs')
        self.record['selected_before'] = str(old)
        self.save()

    def save(self):
        temp = self.folder / 'transaction.json.tmp'
        temp.write_text(json.dumps(self.record, indent=2) + '\n')
        temp.replace(self.folder / 'transaction.json')

    def command(self, args):
        if args[0] == 'dkms':
            # Explicit per-call empty values also defeat inherited reload flags.
            # Do not change the administrator's persistent signing configuration.
            args = [*args, '--dkmstree', TREE, '--sourcetree', '/usr/src', '--installtree', '/lib/modules',
                    '--directive=modprobe_on_install=', '--directive=post_transaction=', '--directive=symlink_modules=']
        self.record['current_command'] = [str(a) for a in args]
        self.save()
        print('Running:', ' '.join(self.record['current_command']), flush=True)
        result = subprocess.run(self.record['current_command'], capture_output=True, text=True, pass_fds=self.fds)
        log = self.folder / ('command-' + str(len(self.record['commands'])) + '.log')
        log.write_text(result.stdout + result.stderr)
        self.record['commands'].append({'argv': self.record.pop('current_command'),
                                       'returncode': result.returncode, 'log': log.name})
        self.save()
        check.require(result.returncode == 0, 'Command failed; see ' + str(log))

    def refresh(self):
        self.command(['depmod', '-a', self.kernel])
        self.command(['update-initramfs', '-u', '-k', self.kernel])

    def install(self):
        members = check.manifest()
        if SOURCE.exists():
            check.root_source(SOURCE)
            check.require(check.sha(SOURCE / 'MANIFEST.sha256') == check.sha(check.ROOT / 'MANIFEST.sha256'),
                          'Different existing source package; refusing to overwrite ' + str(SOURCE))
            check.manifest(SOURCE)
        else:
            SOURCE.mkdir(mode=0o755)
            for name in [*members, 'MANIFEST.sha256']:
                dst = SOURCE / name
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(check.ROOT / name, dst)
                dst.chmod(0o755 if dst.suffix in ('.py', '.sh') else 0o644)
            check.manifest(SOURCE)
        if not (TREE / NAME / VERSION).exists():
            self.command(['dkms', 'add', '-m', NAME, '-v', VERSION])
        registered_source()
        self.command(['dkms', 'build', '-m', NAME, '-v', VERSION, '-k', self.kernel])
        digest = check.before_install(self.kernel, check.INFO['architecture'], TREE)
        self.command(['dkms', 'install', '-m', NAME, '-v', VERSION, '-k', self.kernel])
        self.refresh()  # DKMS ignores post-hook failures, so check explicitly.
        self.record['selected_after'] = check.verify_installed(self.kernel, digest)
        self.record['signed_module_sha256'] = digest

    def remove(self):
        check.require(SOURCE.is_dir() and not SOURCE.is_symlink() and
                      check.sha(SOURCE / 'MANIFEST.sha256') == check.sha(check.ROOT / 'MANIFEST.sha256'),
                      'This release does not own the registered source')
        check.manifest(SOURCE)
        registered_source()
        # Restrict --all to the checked target; don't silently remove later work.
        version_tree = TREE / NAME / VERSION
        check.require(version_tree.is_dir() and all(p.name in {'source', 'build', self.kernel}
                      for p in version_tree.iterdir()), 'Other kernel state exists; remove it explicitly with DKMS')
        self.command(['dkms', 'remove', '-m', NAME, '-v', VERSION, '--all'])
        self.refresh()
        p = selected(self.kernel)
        check.require(check.run(['modinfo', '-F', 'srcversion', p], text=True).stdout.strip() !=
                      check.INFO['supported_kernels'][self.kernel]['srcversion'], 'Removed module is still selected')
        check.verify_image(self.kernel, p, hashlib.sha256(check.module_bytes(p)).hexdigest())
        check.require(not version_tree.exists(), 'DKMS registration remains')
        # Keep source, firmware, key material and the transaction for recovery.
        self.record['selected_after'] = str(p)

    def finish(self):
        live = LIVE.read_text().strip() if LIVE.exists() else None
        check.require(live == self.record['live_before'], 'Loaded driver changed during operation')
        self.record['status'] = 'verified-for-next-normal-boot'
        self.save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['install', 'verify', 'remove', 'firmware'])
    parser.add_argument('--kernel', default=platform.release())
    parser.add_argument('--profile', type=Path, help='Separately supplied exact profile, for firmware action')
    args = parser.parse_args()
    check.require(os.geteuid() == 0, 'Run in your terminal with sudo')
    os.umask(0o022)
    check.manifest()
    check.require(args.kernel in check.INFO['supported_kernels'], 'Unchecked target kernel')
    if args.action != 'remove':
        check.kernel_check(args.kernel)
    if args.action == 'firmware':
        check.require(args.profile and args.profile.is_file() and args.profile.stat().st_size == check.INFO['profile_size']
                      and check.sha(args.profile) == check.INFO['profile_sha256'], 'Wrong or missing profile')
        destination = Path('/lib/firmware') / check.INFO['profile']
        if destination.exists():
            check.firmware_check()
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
            with destination.open('xb') as out:
                out.write(args.profile.read_bytes())
            destination.chmod(0o644)
            check.firmware_check()
        print('Exact firmware installed; no driver or audio change.')
        return
    if args.action == 'verify':
        digest = check.before_install(args.kernel, check.INFO['architecture'], TREE)
        print('Verified:', check.verify_installed(args.kernel, digest))
        return
    check.no_live_reload()
    if args.action == 'install':
        check.hardware_check(); check.firmware_check(); reject_unmanaged_replacement(args.kernel)
    with ExitStack() as stack:
        fds = []
        for path in ['/run/sn8022-binding.lock', '/run/sn8022-dkms-install.lock',
                     '/var/lib/dpkg/lock-frontend', '/var/lib/dpkg/lock']:
            fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
            stack.enter_context(os.fdopen(fd, 'r+'))
            info = os.fstat(fd)
            check.require(stat.S_ISREG(info.st_mode) and info.st_uid == 0, 'Unexpected package/diagnostic lock')
            lock_package(fd) if '/dpkg/' in path else fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fds.append(fd)
        transaction = Transaction(args.kernel, args.action, tuple(fds))
        try:
            transaction.install() if args.action == 'install' else transaction.remove()
            transaction.finish()
        except BaseException as error:
            transaction.record.update(status='needs-inspection', error=str(error))
            transaction.save()
            print('Stopped. Keep the machine running; inspect', transaction.folder, flush=True)
            raise
    print('Verified for the next normal reboot. Current audio is unchanged. Record:', transaction.folder)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit('SN8022: ' + str(error))
