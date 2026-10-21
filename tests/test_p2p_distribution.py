"""A complete P2P distribution must not query or install stock upstream updates."""
import ast
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT))
import run


class DistributionTests(unittest.TestCase):
    def test_marker_stops_network_and_clears_inherited_notice(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(run, 'PORT', Path(tmp)):
            (Path(tmp) / 'p2p-distribution.json').write_text('{"schema_version":1}', encoding='utf-8')
            env = {'BB_UPDATE_AVAILABLE': 'v99.0'}
            with mock.patch('urllib.request.urlopen', side_effect=AssertionError('stock update request')):
                run.update_notice(env, 'v0.6')
            self.assertNotIn('BB_UPDATE_AVAILABLE', env)

    def test_unmarked_default_still_queries_upstream(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(run, 'PORT', Path(tmp)):
            reply = mock.MagicMock()
            reply.__enter__.return_value.read.return_value = b'{"tag_name":"v0.6"}'
            with mock.patch('urllib.request.urlopen', return_value=reply) as request:
                run.update_notice({}, 'v0.6')
            request.assert_called_once()

    def test_gtk_update_callbacks_stop_before_network_or_install(self):
        source = ROOT / 'launcher' / 'sacredfire_launcher.py'
        tree = ast.parse(source.read_text(encoding='utf-8'))
        window = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == 'LauncherWindow')
        selected = [node for node in window.body if isinstance(node, ast.FunctionDef)
                    and node.name in {'check_update', 'on_update'}]
        namespace = {'p2p_distribution': lambda: True, 'tr': lambda text: text}
        exec(compile(ast.Module(body=selected, type_ignores=[]), str(source), 'exec'), namespace)
        row = mock.Mock()
        namespace['check_update'](SimpleNamespace(version_row=row), manual=True)
        row.set_subtitle.assert_called_once()
        namespace['on_update'](SimpleNamespace(), None)

    def test_packaged_launcher_name_is_stable_only_for_p2p_builds(self):
        path = ROOT / 'packaging' / 'windows' / 'make_portable.py'
        for flag, expected in [('ON', 'CTR-NitroFueled-PC.exe'), ('OFF', 'Crash\u2122 Team Racing Nitro-Fueled.exe')]:
            with self.subTest(flag=flag), mock.patch.dict(os.environ, {'BB_P2P_DISTRIBUTION': flag}):
                spec = importlib.util.spec_from_file_location('portable_fixture', path)
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                self.assertEqual(module.PACKAGED_LAUNCHER, expected)


if __name__ == '__main__':
    unittest.main()
