"""Private-cache preservation tests using synthetic bytes, never game shaders."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from ultrawide_local_shaders import update_shaders


def correct(data, factor):
    return data + f' correction {factor}'.encode()


def known(data, original):
    return data in (original, correct(original, 0.5), correct(original, 0.75))


class LocalShaderTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.cache, self.backup = self.root / 'cache', self.root / 'originals'
        self.cache.mkdir()
        self.originals = {'12345678': b'first synthetic shader',
                          '87654321': b'second synthetic shader'}
        self.signatures = {key: hashlib.sha256(data).hexdigest()
                           for key, data in self.originals.items()}

    def path(self, folder, key):
        return folder / f'0x00000000{key}_0.spv'

    def seed(self):
        for key, data in self.originals.items():
            self.path(self.cache, key).write_bytes(data)

    def apply(self, enabled=True, factor=0.5):
        update_shaders(self.cache, self.backup, enabled, factor, self.signatures,
                       correct, known)

    def test_aspect_switch_preserves_original_and_disable_restores(self):
        self.seed()
        self.apply()
        self.apply(factor=0.75)
        for key, original in self.originals.items():
            self.assertEqual(self.path(self.cache, key).read_bytes(), correct(original, 0.75))
            self.assertEqual(self.path(self.backup, key).read_bytes(), original)
        self.apply(False)
        for key, original in self.originals.items():
            self.assertEqual(self.path(self.cache, key).read_bytes(), original)

    def test_second_unknown_original_leaves_all_files_unchanged(self):
        self.seed()
        self.path(self.cache, '87654321').write_bytes(b'another mod')
        before = {p.name: p.read_bytes() for p in self.cache.iterdir()}
        with self.assertRaises(RuntimeError):
            self.apply()
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.cache.iterdir()})
        self.assertFalse(self.backup.exists())

    def test_foreign_change_is_preserved_on_disable(self):
        self.seed()
        self.apply()
        self.path(self.cache, '87654321').write_bytes(b'foreign modification')
        before = {p.name: p.read_bytes() for p in self.cache.iterdir()}
        with self.assertRaises(RuntimeError):
            self.apply(False)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.cache.iterdir()})

    def test_invalid_saved_original_is_not_overwritten(self):
        self.seed()
        self.apply()
        self.path(self.backup, '12345678').write_bytes(b'corrupted backup')
        with self.assertRaises(RuntimeError):
            self.apply()
        self.assertEqual(self.path(self.backup, '12345678').read_bytes(), b'corrupted backup')

    def test_disabled_cold_cache_needs_no_distributed_shader(self):
        self.apply(False)
        self.assertFalse(self.backup.exists())
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_enabled_missing_cache_creates_no_backup(self):
        with self.assertRaises(RuntimeError):
            self.apply()
        self.assertFalse(self.backup.exists())


if __name__ == '__main__':
    unittest.main()
