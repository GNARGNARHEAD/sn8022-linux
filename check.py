#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Portable DKMS checks. Never reads, creates or copies a private signing key."""
import argparse
import gzip
import hashlib
import json
import lzma
import os
from pathlib import Path, PurePosixPath
import platform
import re
import struct
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
INFO = json.loads((ROOT / 'release.json').read_text())
MODULE = INFO['module']
SUFFIXES = ('.ko', '.ko.zst', '.ko.xz', '.ko.gz')
MAGIC = b'~Module signature appended~\n'


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, **kwargs)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def manifest(folder=ROOT):
    members = []
    for line in (folder / 'MANIFEST.sha256').read_text().splitlines():
        digest, name = line.split('  ', 1)
        rel = PurePosixPath(name)
        require(re.fullmatch('[0-9a-f]{64}', digest) and name not in members and
                not rel.is_absolute() and '..' not in rel.parts, 'Invalid source manifest')
        path = folder / name
        require(not path.is_symlink() and path.resolve().is_relative_to(folder.resolve()) and
                path.is_file() and sha(path) == digest, 'Source changed: ' + name)
        members.append(name)
    require(members, 'Empty source manifest')
    return members


def kernel_check(kernel):
    require(platform.machine() == INFO['architecture'], 'Unsupported architecture')
    require(kernel in INFO['supported_kernels'],
            'Kernel has not been checked for private HDA-header compatibility: ' + kernel)
    build = Path('/lib/modules') / kernel / 'build'
    for name, digest in INFO['supported_kernels'][kernel]['headers'].items():
        require(sha(build / name) == digest, 'Kernel build inputs differ: ' + name)


def hardware_check():
    matches = []
    for codec in Path('/sys/bus/hdaudio/devices').glob('hdaudioC*D*'):
        if ((codec / 'vendor_id').read_text().strip() == INFO['codec_vendor_id'] and
                (codec / 'subsystem_id').read_text().strip() == INFO['codec_subsystem_id']):
            matches.append(codec)
    require(matches, 'The supported codec/subsystem was not found')


def firmware_check():
    path = Path('/lib/firmware') / INFO['profile']
    require(path.is_file() and path.stat().st_size == INFO['profile_size'] and
            sha(path) == INFO['profile_sha256'], 'Install the exact separately supplied amplifier profile first')


def no_live_reload():
    # These are the administrator's own files, already sourced by DKMS. Inspect
    # only its effective reload switch; never print signing variables or keys.
    require(run(['dkms', '--version'], text=True).stdout.strip() == 'dkms-3.2.2',
            'This package has checked DKMS 3.2.2 semantics; review another version before use')
    require(not list(Path('/etc/dkms').glob('sn8022*.conf')),
            'Package-specific DKMS overrides exist; review before using this package')
    result = subprocess.run(['bash', '--noprofile', '--norc', str(ROOT / 'framework-policy.sh')],
                            capture_output=True, env={**os.environ, 'BASH_ENV': '/dev/null'})
    require(result.returncode == 0,
            'Unsupported DKMS policy: custom trees, symlink modules, directives or live/transaction hooks; inspect framework configuration')


def root_source(folder):
    members = manifest(folder)
    paths = {folder, folder / 'MANIFEST.sha256'}
    for name in members:
        path = folder / name
        paths.add(path)
        paths.update(p for p in path.parents if p.is_relative_to(folder))
    for path in paths:
        info = path.lstat()
        require(not path.is_symlink() and info.st_uid == 0 and not stat.S_IMODE(info.st_mode) & 0o022,
                'Registered source must be root-owned and not group/world writable: ' + str(path))


def image_path(path, root):
    require(not path.is_symlink() and path.resolve().is_relative_to(root.resolve()),
            'Unexpected image payload symlink')
    parts = path.relative_to(root).parts
    if parts[0] == 'main' or re.fullmatch(r'early\d*', parts[0]):
        parts = parts[1:]
    return PurePosixPath(*parts)


def secure_boot():
    result = subprocess.run(['mokutil', '--sb-state'], capture_output=True, text=True)
    if result.returncode == 0 and result.stdout.strip() == 'SecureBoot enabled':
        return True
    if result.returncode == 0 and result.stdout.strip() == 'SecureBoot disabled':
        return False
    require(False, 'Cannot establish Secure Boot state; inspect mokutil --sb-state')


def module_bytes(path):
    if path.suffix == '.zst':
        return run(['zstd', '-q', '-d', '-c', path]).stdout
    if path.suffix == '.xz':
        return lzma.decompress(path.read_bytes())
    if path.suffix == '.gz':
        return gzip.decompress(path.read_bytes())
    return path.read_bytes()


def verify_module(path, kernel, trust):
    data = module_bytes(path)
    with tempfile.TemporaryDirectory(prefix='sn8022-signature-') as directory:
        temp = Path(directory)
        plain = temp / 'module.ko'
        plain.write_bytes(data)
        expected = {'name': MODULE.replace('-', '_'),
                    'srcversion': INFO['supported_kernels'][kernel]['srcversion'],
                    'firmware': INFO['profile']}
        for field, value in expected.items():
            require(run(['modinfo', '-F', field, plain], text=True).stdout.strip() == value,
                    'Built module identity differs: ' + field)
        require(run(['modinfo', '-F', 'vermagic', plain], text=True).stdout.startswith(kernel + ' '),
                'Module targets another kernel')
        if trust:
            require(data.endswith(MAGIC) and len(data) > len(MAGIC) + 12, 'Secure Boot requires a signed module')
            desc = struct.unpack('>BBBBB3sI', data[-len(MAGIC)-12:-len(MAGIC)])
            require(desc[:6] == (0, 0, 2, 0, 0, b'\0\0\0'), 'Unexpected module signature format')
            end = len(data) - len(MAGIC) - 12
            size = desc[-1]
            require(0 < size < end, 'Invalid module signature size')
            (temp / 'payload').write_bytes(data[:end-size])
            (temp / 'signature').write_bytes(data[end-size:end])
            run(['mokutil', '--export'], cwd=temp)
            verified = False
            for cert in temp.glob('*.der'):
                conversion = subprocess.run(['openssl', 'x509', '-inform', 'DER', '-in', str(cert),
                                             '-out', str(temp / 'cert.pem')], capture_output=True)
                if conversion.returncode:
                    continue
                result = subprocess.run(['openssl', 'cms', '-verify', '-binary', '-inform', 'DER',
                    '-in', str(temp / 'signature'), '-content', str(temp / 'payload'),
                    '-certfile', str(temp / 'cert.pem'), '-nointern', '-noverify',
                    '-no-CAfile', '-no-CApath', '-no-CAstore', '-out', os.devnull], capture_output=True)
                if result.returncode == 0:
                    verified = True
                    break
            require(verified, 'Module was not verified with an enrolled MOK; enroll your DKMS public certificate, reboot, and retry')
    return hashlib.sha256(data).hexdigest()


def built_module(tree, kernel, arch):
    folder = Path(tree) / INFO['name'] / INFO['version'] / kernel / arch / 'module'
    paths = [folder / (MODULE + suffix) for suffix in SUFFIXES if (folder / (MODULE + suffix)).is_file()]
    require(len(paths) == 1, 'Expected one DKMS-built module')
    return paths[0]


def before_install(kernel, arch, tree):
    manifest(); kernel_check(kernel); hardware_check(); firmware_check(); no_live_reload()
    require(arch == INFO['architecture'], 'Unsupported DKMS architecture')
    return verify_module(built_module(tree, kernel, arch), kernel, secure_boot())


def verify_installed(kernel, expected):
    selected = Path(run(['modinfo', '-k', kernel, '-F', 'filename', MODULE], text=True).stdout.strip())
    require(selected.parent.resolve() == (Path('/lib/modules') / kernel / 'updates/dkms').resolve(),
            'DKMS module is not selected')
    require(verify_module(selected, kernel, secure_boot()) == expected, 'Selected module differs from verified build')
    verify_image(kernel, selected, expected, require_profile=True)
    return str(selected)


def verify_image(kernel, selected, expected, require_profile=False):
    image = Path('/boot') / ('initrd.img-' + kernel)
    require(image.is_file(), 'Expected an existing Ubuntu initramfs')
    with tempfile.TemporaryDirectory(prefix='sn8022-initramfs-') as directory:
        target = Path(directory) / 'extracted'
        run(['unmkinitramfs', image, target])
        relative = selected.resolve().relative_to((Path('/lib/modules') / kernel).resolve())
        verify_image_tree(target, kernel, relative, expected, require_profile)


def verify_image_tree(target, kernel, relative, expected, require_profile=False):
    modules = [p for p in target.rglob('*') if p.name.replace('_', '-') in {MODULE + s for s in SUFFIXES}]
    allowed_modules = {PurePosixPath(prefix) / 'modules' / kernel / relative
                       for prefix in ('lib', 'usr/lib')}
    for path in modules:
        require(image_path(path, target) in allowed_modules and
                hashlib.sha256(module_bytes(path)).hexdigest() == expected,
                'Stale or unexpected Conexant module in initramfs')
    profiles = list(target.rglob(Path(INFO['profile']).name))
    require(not require_profile or not modules or profiles, 'Initramfs module lacks its firmware')
    for path in profiles:
        require(image_path(path, target) in {PurePosixPath(prefix) / 'firmware' / INFO['profile']
                for prefix in ('lib', 'usr/lib')} and sha(path) == INFO['profile_sha256'],
                'Unexpected amplifier firmware in initramfs')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['build', 'before-install'])
    parser.add_argument('kernel')
    parser.add_argument('arch', nargs='?')
    parser.add_argument('tree', nargs='?')
    args = parser.parse_args()
    manifest(); kernel_check(args.kernel)
    if args.mode == 'before-install':
        require(args.arch and args.tree, 'DKMS arch and tree are required')
        print('Verified module:', before_install(args.kernel, args.arch, args.tree))
    else:
        print('Source and supported kernel build inputs verified.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit('SN8022 check failed: ' + str(error))
