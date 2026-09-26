# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline checks. No root, actual DKMS install, audio or firmware required."""
from contextlib import ExitStack
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check
import manage

KERNEL = next(iter(check.INFO['supported_kernels']))


def put(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def manifest_for(folder):
    names = sorted(str(p.relative_to(folder)) for p in folder.rglob('*') if p.is_file() and p.name != 'MANIFEST.sha256')
    (folder / 'MANIFEST.sha256').write_text(''.join(check.sha(folder / n) + '  ' + n + '\n' for n in names))


class ManifestTests(unittest.TestCase):
    def test_changed_source_and_escape_are_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            put(root / 'good.c', b'good')
            manifest_for(root)
            self.assertEqual(check.manifest(root), ['good.c'])
            (root / 'good.c').write_bytes(b'changed')
            with self.assertRaisesRegex(RuntimeError, 'Source changed'):
                check.manifest(root)
            (root / 'MANIFEST.sha256').write_text('0' * 64 + '  ../outside\n')
            with self.assertRaisesRegex(RuntimeError, 'Invalid source manifest'):
                check.manifest(root)

    def test_symlink_and_duplicate_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            put(root / 'good', b'good')
            (root / 'link').symlink_to('good')
            manifest_for(root)
            with self.assertRaisesRegex(RuntimeError, 'Source changed'):
                check.manifest(root)
            (root / 'link').unlink()
            manifest_for(root)
            m = root / 'MANIFEST.sha256'
            m.write_text(m.read_text() * 2)
            with self.assertRaisesRegex(RuntimeError, 'Invalid source manifest'):
                check.manifest(root)

    def test_unprivileged_source_rejected(self):
        if os.geteuid() == 0:
            self.skipTest('Run the offline suite without sudo')
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            put(root / 'good', b'good')
            manifest_for(root)
            with self.assertRaisesRegex(RuntimeError, 'root-owned'):
                check.root_source(root)


class FrameworkTests(unittest.TestCase):
    def policy(self, configs=(), environment=None):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            (root / 'framework.conf.d').mkdir()
            for i, config in enumerate(configs):
                (root / 'framework.conf.d' / (str(i) + '.conf')).write_text(config)
            # Run the actual shell checker against fixture paths, never /etc.
            script = (check.ROOT / 'framework-policy.sh').read_text().replace('/etc/dkms/', str(root) + '/')
            return subprocess.run(['bash', '--noprofile', '--norc'], input=script, text=True,
                capture_output=True, env={'PATH': os.environ['PATH'], **(environment or {})}).returncode

    def test_default_and_signing_paths_are_accepted_without_reading_keys(self):
        self.assertEqual(self.policy(), 0)
        self.assertEqual(self.policy(['mok_signing_key=/does/not/exist\nmok_certificate=/does/not/exist\n']), 0)

    def test_later_unset_does_not_clear_prior_hook(self):
        self.assertNotEqual(self.policy(['modprobe_on_install=yes\n', 'unset modprobe_on_install\n']), 0)
        self.assertEqual(self.policy(['modprobe_on_install=yes\n', 'modprobe_on_install=\n']), 0)

    def test_conditional_uses_retained_framework_values(self):
        self.assertNotEqual(self.policy(['parallel_jobs=2\n',
            'if [ "${parallel_jobs-}" = 2 ]; then post_transaction=unexpected; fi\n']), 0)

    def test_false_is_nonempty_to_dkms(self):
        for setting in ('modprobe_on_install', 'post_transaction', 'symlink_modules'):
            with self.subTest(setting=setting):
                self.assertNotEqual(self.policy([setting + '=false\n']), 0)

    def test_directives_and_custom_trees_rejected(self):
        self.assertNotEqual(self.policy(['DKMS_DIRECTIVE_0="POST_INSTALL=unexpected"\n']), 0)
        self.assertNotEqual(self.policy(environment={'DKMS_DIRECTIVE_0': 'POST_INSTALL=unexpected'}), 0)
        for setting in ('dkms_tree', 'source_tree', 'install_tree'):
            with self.subTest(setting=setting):
                self.assertNotEqual(self.policy([setting + '=/nondefault\n']), 0)


class ImageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.relative = Path('updates/dkms') / (check.MODULE + '.ko')
        self.module = put(self.root / 'main/usr/lib/modules' / KERNEL / self.relative, b'fixture module')
        self.profile = put(self.root / 'main/usr/lib/firmware' / check.INFO['profile'], b'fixture profile')
        self.info = {**check.INFO, 'profile_sha256': check.sha(self.profile)}
        self.digest = check.sha(self.module)

    def verify(self):
        with patch.object(check, 'INFO', self.info):
            check.verify_image_tree(self.root, KERNEL, self.relative, self.digest, True)

    def test_exact_locations_and_contents(self):
        self.verify()
        self.module.write_bytes(b'stale module')
        with self.assertRaisesRegex(RuntimeError, 'Conexant module'):
            self.verify()

    def test_wrong_firmware_location_is_not_presence(self):
        bad = self.root / 'main/etc/firmware' / check.INFO['profile']
        bad.parent.mkdir(parents=True)
        self.profile.rename(bad)
        with self.assertRaisesRegex(RuntimeError, 'Unexpected amplifier firmware'):
            self.verify()

    def test_missing_profile_and_duplicate_stale_module(self):
        self.profile.unlink()
        with self.assertRaisesRegex(RuntimeError, 'lacks its firmware'):
            self.verify()
        self.profile.write_bytes(b'fixture profile')
        put(self.root / 'early/lib/modules' / KERNEL / self.relative, b'stale')
        with self.assertRaisesRegex(RuntimeError, 'Conexant module'):
            self.verify()

    def test_wrong_kernel_path_and_symlink(self):
        wrong = self.root / 'main/usr/lib/modules/unchecked' / self.relative
        wrong.parent.mkdir(parents=True)
        self.module.rename(wrong)
        with self.assertRaisesRegex(RuntimeError, 'Conexant module'):
            self.verify()
        self.module.symlink_to(wrong)
        with self.assertRaisesRegex(RuntimeError, 'symlink'):
            check.image_path(self.module, self.root)

    def test_no_embedded_module_is_valid(self):
        self.module.unlink()
        self.profile.unlink()
        self.verify()


class LifecycleTests(unittest.TestCase):
    """Actual transaction methods with simulated DKMS and boot-image operations.

    This verifies ordering and failure boundaries, not privileged installation.
    """
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = self.root / 'package'
        put(self.package / 'driver.c', b'driver source')
        manifest_for(self.package)
        self.source = self.root / 'usr/src' / ('sn8022-' + manage.VERSION)
        self.source.parent.mkdir(parents=True)
        self.tree = self.root / 'dkms'
        self.version_tree = self.tree / manage.NAME / manage.VERSION
        self.output = put(self.root / (check.MODULE + '.ko'), b'new module')
        self.calls = []
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        for obj, name, value in ((check, 'ROOT', self.package), (manage, 'SOURCE', self.source),
                                 (manage, 'TREE', self.tree)):
            self.stack.enter_context(patch.object(obj, name, value))
        # The test has no root-owned files; test ownership refusal separately.
        self.stack.enter_context(patch.object(check, 'root_source', side_effect=lambda p: check.manifest(p)))
        self.stack.enter_context(patch.object(check, 'manifest', wraps=check.manifest))
        # manifest's default argument was bound to the actual package at import.
        original_manifest = check.manifest._mock_wraps
        check.manifest.side_effect = lambda p=None: original_manifest(self.package if p is None else p)
        self.stack.enter_context(patch.object(check, 'before_install', side_effect=self.preinstall))
        self.stack.enter_context(patch.object(check, 'verify_installed', side_effect=self.verify))
        self.stack.enter_context(patch.object(manage, 'selected', return_value=self.output))
        self.stack.enter_context(patch.object(check, 'verify_image', side_effect=lambda *a: self.calls.append('image-verified')))
        self.stack.enter_context(patch.object(check, 'run', return_value=subprocess.CompletedProcess([], 0, 'STOCK\n', '')))
        self.stack.enter_context(patch.object(subprocess, 'run', side_effect=self.command))
        self.tx = manage.Transaction.__new__(manage.Transaction)
        self.tx.folder = self.root / 'transaction'
        self.tx.folder.mkdir()
        self.tx.kernel, self.tx.fds = KERNEL, ()
        self.tx.record = {'commands': []}
        self.fail = None

    def command(self, args, **kwargs):
        verb = args[1] if args[0] == 'dkms' else args[0]
        self.calls.append(verb)
        if self.fail == verb:
            return subprocess.CompletedProcess(args, 1, '', 'injected failure')
        if verb == 'add':
            self.version_tree.mkdir(parents=True)
            (self.version_tree / 'source').symlink_to(self.source)
        if verb == 'remove':
            shutil.rmtree(self.version_tree)
        return subprocess.CompletedProcess(args, 0, '', '')

    def preinstall(self, *args):
        self.calls.append('preinstall')
        if self.fail == 'preinstall':
            raise RuntimeError('untrusted module')
        return 'module-digest'

    def verify(self, *args):
        self.calls.append('verified')
        if self.fail == 'verified':
            raise RuntimeError('stale image')
        return str(self.output)

    def test_clean_install_and_no_reload_directives(self):
        self.tx.install()
        self.assertEqual(self.calls, ['add', 'build', 'preinstall', 'install', 'depmod', 'update-initramfs', 'verified'])
        self.assertEqual((self.source / 'driver.c').read_bytes(), b'driver source')
        for call in self.tx.record['commands']:
            if call['argv'][0] == 'dkms':
                for flag in ('modprobe_on_install', 'post_transaction', 'symlink_modules'):
                    self.assertIn('--directive=' + flag + '=', call['argv'])
        self.assertFalse(any('modprobe' == c['argv'][0] for c in self.tx.record['commands']))

    def test_build_or_trust_failure_cannot_install(self):
        for failure in ('build', 'preinstall'):
            self.fail = failure
            self.calls.clear()
            with self.assertRaises(RuntimeError):
                self.tx.install()
            self.assertNotIn('install', self.calls)
            self.assertNotIn('update-initramfs', self.calls)

    def test_boot_image_failure_cannot_claim_verified(self):
        self.fail = 'update-initramfs'
        with self.assertRaises(RuntimeError):
            self.tx.install()
        self.assertNotIn('verified', self.calls)
        self.assertEqual(self.tx.record['commands'][-1]['returncode'], 1)
        self.assertNotIn('selected_after', self.tx.record)

    def test_wrong_registration_rejected_before_build_or_remove(self):
        shutil.copytree(self.package, self.source)
        self.version_tree.mkdir(parents=True)
        (self.version_tree / 'source').symlink_to(self.package)
        for action in (self.tx.install, self.tx.remove):
            with self.assertRaisesRegex(RuntimeError, 'registration points'):
                action()
        self.assertEqual(self.calls, [])

    def test_existing_version_install_retains_rollback_source(self):
        old = put(self.tree / 'sn8022/0.2.0/saved-module', b'known good')
        self.tx.install()
        self.calls.clear()
        self.tx.install()
        self.assertNotIn('add', self.calls)
        self.assertNotIn('remove', self.calls)
        self.assertEqual(old.read_bytes(), b'known good')

    def test_remove_does_not_build_or_require_hardware_headers(self):
        self.tx.install()
        self.calls.clear()
        with patch.object(check, 'kernel_check', side_effect=AssertionError('headers not available')), \
                patch.object(check, 'hardware_check', side_effect=AssertionError('hardware not present')):
            self.tx.remove()
        self.assertEqual(self.calls, ['remove', 'depmod', 'update-initramfs', 'image-verified'])
        self.assertTrue(self.source.is_dir())
        self.assertFalse(self.version_tree.exists())

    def test_remove_refuses_additional_kernel_state(self):
        self.tx.install()
        (self.version_tree / 'another-kernel').mkdir()
        self.calls.clear()
        with self.assertRaisesRegex(RuntimeError, 'Other kernel state'):
            self.tx.remove()
        self.assertEqual(self.calls, [])


if __name__ == '__main__':
    unittest.main()
