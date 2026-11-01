import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
import time
import unittest
from unittest import mock

from test_plugin import LIB_PATH, bind_library

ROOT = Path(__file__).resolve().parent
OFFLINE = '''import io, urllib.request
urllib.request.build_opener = lambda *a, **kw: type('OfflineOpener', (), {
    'open': lambda self, *a, **kw: io.BytesIO(b'{"ip":"192.0.2.1","timezone":"Etc/UTC"}')
})()
'''


class TestEmbeddedRefresh(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='tz-embed-', dir='/tmp' if os.name != 'nt' else None)
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.data = self.base / 'data with spaces'
        self.state = self.data / 'codex-chronos.state'
        self.home = self.base / 'home'
        self.home.mkdir()
        modules = self.base / 'modules'
        modules.mkdir()
        (modules / 'sitecustomize.py').write_text(OFFLINE)
        env = {k: v for k, v in os.environ.items() if not k.startswith('CODEX_TZ_')}
        env.update(HOME=str(self.home), USERPROFILE=str(self.home),
                   LOCALAPPDATA=str(self.home), APPDATA=str(self.home),
                   PYTHONPATH=str(modules), CODEX_TZ_DATA_DIR=str(self.data),
                   CODEX_TZ_PYTHON=sys.executable, CODEX_TZ_PROXY='direct',
                   CODEX_TZ_REFRESH_INTERVAL='0')
        self.environment = mock.patch.dict(os.environ, env, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.library = self.base / LIB_PATH.name
        shutil.copyfile(LIB_PATH, self.library)
        self.api, self.call = bind_library(self.library)
        if os.name == 'nt':
            from _ctypes import FreeLibrary
            self.addCleanup(FreeLibrary, self.call._library._handle)
        self.addCleanup(self.api.shutdown)

    def management(self, endpoint, method='GET'):
        response = self.call('management.handle', {
            'Path': '/v0/management/plugins/codex-chronos/' + endpoint,
            'Method': method})['result']
        return response['StatusCode'], json.loads(base64.b64decode(response['Body']))

    def refresh(self):
        code, body = self.management('egress/refresh', 'POST')
        self.assertEqual((code, body.get('ok')), (200, True), body)
        self.assertEqual(self.management('status')[1]['timezone'], 'Etc/UTC')

    def embedded_path(self):
        digest = hashlib.sha256((ROOT / 'refresh_egress.py').read_bytes()).hexdigest()
        return self.data / ('refresh_egress-' + digest + '.py')

    def test_library_only_install_refreshes_without_external_script(self):
        self.assertEqual(list(self.base.glob('*.py')), [])
        self.refresh()
        self.assertEqual(self.embedded_path().read_bytes(), (ROOT / 'refresh_egress.py').read_bytes())
        self.assertTrue(self.state.is_file())
        if os.name != 'nt':
            self.assertEqual(self.embedded_path().stat().st_mode & 0o777, 0o600)

    def test_custom_config_directory_and_state_override(self):
        os.environ.pop('CODEX_TZ_DATA_DIR')
        self.call('plugin.reconfigure', {'config_yaml': 'data_dir: ' + json.dumps(str(self.data)) + '\n'})
        target = self.base / 'separate state' / 'custom.state'
        os.environ['CODEX_TZ_STATE_FILE'] = str(target)
        self.refresh()
        self.assertTrue(target.is_file())
        self.assertFalse(self.state.exists())
        self.assertTrue(self.embedded_path().is_file())

    def test_corrupt_embedded_copy_is_repaired(self):
        self.refresh()
        self.embedded_path().write_text('raise SystemExit(99)')
        self.state.unlink()
        self.refresh()
        self.assertEqual(self.embedded_path().read_bytes(), (ROOT / 'refresh_egress.py').read_bytes())

    def test_old_embedded_copy_is_not_reused(self):
        self.data.mkdir()
        old = self.data / ('refresh_egress-' + '0' * 64 + '.py')
        old.write_text('raise SystemExit(99)')
        self.refresh()
        self.assertNotEqual(old, self.embedded_path())
        self.assertEqual(old.read_text(), 'raise SystemExit(99)')

    def test_external_helper_keeps_precedence(self):
        self.data.mkdir()
        custom = self.data / 'refresh_egress.py'
        source = 'raise SystemExit(42)\n'
        custom.write_text(source)
        code, body = self.management('egress/refresh', 'POST')
        self.assertEqual((code, body['ok']), (503, False))
        self.assertEqual(custom.read_text(), source)
        self.assertFalse(self.embedded_path().exists())

    def test_background_refresh_uses_embedded_script(self):
        os.environ['CODEX_TZ_REFRESH_INTERVAL'] = '900'
        self.assertFalse(self.management('status')[1]['available'])
        deadline = time.monotonic() + 10
        while not self.state.exists() and time.monotonic() < deadline:
            time.sleep(.05)
        self.assertTrue(self.state.is_file(), 'embedded background helper never wrote state')
        self.assertEqual(self.management('status')[1]['timezone'], 'Etc/UTC')

    def test_concurrent_refreshes_publish_complete_script(self):
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: self.management('egress/refresh', 'POST'), range(4)))
        for code, body in results:
            self.assertEqual((code, body.get('ok')), (200, True), body)
        self.assertEqual(self.embedded_path().read_bytes(), (ROOT / 'refresh_egress.py').read_bytes())
        self.assertEqual(list(self.data.glob('.refresh-*')), [])

    def test_custom_config_directory_without_state_override(self):
        os.environ.pop('CODEX_TZ_DATA_DIR')
        self.call('plugin.reconfigure', {'config_yaml': 'data_dir: ' + json.dumps(str(self.data)) + '\n'})
        self.refresh()
        self.assertTrue(self.state.is_file())

    @unittest.skipIf(os.name == 'nt', 'POSIX symlink replacement')
    def test_embedded_symlink_is_replaced_without_touching_target(self):
        self.data.mkdir()
        target = self.base / 'external.py'
        source = 'raise SystemExit(99)'
        target.write_text(source)
        self.embedded_path().symlink_to(target)
        self.refresh()
        self.assertFalse(self.embedded_path().is_symlink())
        self.assertEqual(target.read_text(), source)

    def test_unwritable_embedded_destination_reports_failure(self):
        self.data.write_text('not a directory')
        code, body = self.management('egress/refresh', 'POST')
        self.assertEqual((code, body['ok']), (503, False))
        self.assertIn('Python', body['reason'])
        self.assertIn('helper.log', body['reason'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
