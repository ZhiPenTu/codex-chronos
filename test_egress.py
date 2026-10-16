#!/usr/bin/env python3
"""Offline host-helper contract tests: verify that the probe updates state."""
import importlib.util
import io
import os
import pathlib
import socket
import socketserver
import tempfile
import threading
import time
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("refresh_egress", ROOT / "refresh_egress.py")
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)

class FakeOpener:
    def __init__(self, text):
        self.text=text
    def open(self, request, timeout):
        assert timeout==12
        assert request.full_url=="https://ipinfo.io/json"
        return io.BytesIO(self.text.encode("utf-8"))

class TestHostEgress(unittest.TestCase):
    def test_refresh_updates_iana_zone_over_proxy(self):
        with tempfile.TemporaryDirectory() as path:
            with mock.patch.object(helper, "STATE", pathlib.Path(path)/"state"), \
                 mock.patch.object(helper, "resolve_proxy", return_value="http://proxy:7890"), \
                 mock.patch.object(helper.urllib.request, "build_opener", return_value=FakeOpener('{"ip":"203.0.113.22","timezone":"America/Los_Angeles"}')) as opener:
                helper.refresh()
                observed=helper.STATE.read_text()
                self.assertIn("America/Los_Angeles|",observed)
                self.assertIn("203.0.113.22",observed)
                opener.assert_called_once()

    @unittest.skipIf(os.name == 'nt', 'Windows uses file-mailbox transport')
    def test_unix_socket_helper_refresh_command(self):
        with tempfile.TemporaryDirectory() as path:
            sock=str(pathlib.Path(path)/"test.sock")
            with mock.patch.object(helper,"refresh") as refresh:
                with socketserver.ThreadingUnixStreamServer(sock,helper.RefreshHandler) as server:
                    thread=threading.Thread(target=server.serve_forever,daemon=True)
                    thread.start()
                    try:
                        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as client:
                            client.connect(sock)
                            client.sendall(b"REFRESH\n")
                            self.assertEqual(client.recv(8),b"OK\n")
                    finally:
                        server.shutdown()
                        thread.join(timeout=3)
                refresh.assert_called_once()

    def test_windows_mailbox_helper_refresh_command(self):
        with tempfile.TemporaryDirectory() as path:
            target = pathlib.Path(path)/'refresh.sock'
            nonce = '0123456789abcdef'
            pathlib.Path(str(target)+'.request').write_text(nonce+'\n')
            with mock.patch.object(helper,'refresh') as refresh:
                self.assertTrue(helper.windows_poll_once(target))
                self.assertEqual(pathlib.Path(str(target)+'.'+nonce+'.reply').read_text(), 'OK\n')
                self.assertFalse(helper.windows_poll_once(target))
                refresh.assert_called_once()

class TestDesktopPaths(unittest.TestCase):
    """The helper must resolve the same data directory as the C plugin."""

    def test_data_dir_env_override(self):
        with tempfile.TemporaryDirectory() as path:
            with mock.patch.dict(os.environ, {'CODEX_TZ_DATA_DIR': path}):
                self.assertEqual(helper.data_dir(), pathlib.Path(path))

    def test_data_dir_container_when_present(self):
        with mock.patch.dict(os.environ), \
             mock.patch.object(pathlib.Path, 'is_dir', return_value=True):
            os.environ.pop('CODEX_TZ_DATA_DIR', None)
            self.assertEqual(helper.data_dir(), pathlib.Path('/CLIProxyAPI/data'))

    def test_data_dir_desktop_fallback(self):
        with tempfile.TemporaryDirectory() as home:
            with mock.patch.dict(os.environ, clear=True) as env:
                env['HOME'] = home
                env['USERPROFILE'] = home
                with mock.patch.object(pathlib.Path, 'is_dir', return_value=False):
                    resolved = helper.data_dir()
            self.assertTrue(resolved.is_absolute())
            # macOS uses a dot-prefixed directory; Linux uses .local/share.
            self.assertTrue(str(resolved).endswith('codex-chronos'))

    def test_state_and_socket_follow_data_dir(self):
        with tempfile.TemporaryDirectory() as base:
            with mock.patch.dict(os.environ, {'CODEX_TZ_DATA_DIR': base}):
                self.assertEqual(helper.state_path(),
                                 pathlib.Path(base) / 'codex-chronos.state')
                self.assertEqual(helper.refresh_socket_path(),
                                 pathlib.Path(base) / 'codex-chronos-refresh.sock')

    def test_per_file_env_overrides_win(self):
        with tempfile.TemporaryDirectory() as base:
            with mock.patch.dict(os.environ, {'CODEX_TZ_DATA_DIR': base,
                                              'CODEX_TZ_STATE': '/tmp/custom.state',
                                              'CODEX_TZ_REFRESH_SOCKET': '/tmp/custom.sock'}):
                self.assertEqual(helper.state_path(), pathlib.Path('/tmp/custom.state'))
                self.assertEqual(helper.refresh_socket_path(), pathlib.Path('/tmp/custom.sock'))


class TestProxyResolution(unittest.TestCase):
    """Desktop runs must not depend on docker or a mihomo sidecar."""

    def _clean_env(self):
        env = {k: v for k, v in os.environ.items()
               if k not in ('CODEX_TZ_PROXY', 'HTTPS_PROXY', 'https_proxy')}
        return mock.patch.dict(os.environ, env, clear=True)

    def test_explicit_direct(self):
        for value in ('direct', 'DIRECT', 'none', 'off', ''):
            with mock.patch.dict(os.environ, {'CODEX_TZ_PROXY': value}):
                self.assertIsNone(helper.resolve_proxy())

    def test_explicit_url(self):
        with mock.patch.dict(os.environ, {'CODEX_TZ_PROXY': ' http://127.0.0.1:9999 '}):
            self.assertEqual(helper.resolve_proxy(), 'http://127.0.0.1:9999')

    def test_desktop_local_proxy_detection(self):
        class FakeConn:
            def __enter__(self):
                return self
            def __exit__(self, *args):
                return False
        with self._clean_env(), \
             mock.patch.object(helper.subprocess, 'run',
                               side_effect=FileNotFoundError('no docker')):
            def fake_connect(address, timeout=None):
                if address[1] == 7897:
                    return FakeConn()
                raise OSError('refused')
            with mock.patch.object(helper.socket, 'create_connection',
                                   side_effect=fake_connect):
                self.assertEqual(helper.resolve_proxy(), 'http://127.0.0.1:7897')

    def test_desktop_direct_fallback(self):
        with self._clean_env(), \
             mock.patch.object(helper.subprocess, 'run',
                               side_effect=FileNotFoundError('no docker')), \
             mock.patch.object(helper.socket, 'create_connection',
                               side_effect=OSError('refused')):
            self.assertIsNone(helper.resolve_proxy())

    def test_container_mihomo_sidecar(self):
        with self._clean_env(), \
             mock.patch.object(helper.subprocess, 'run') as run, \
             mock.patch.object(helper.socket, 'create_connection',
                               side_effect=OSError('refused')):
            run.return_value = mock.Mock(stdout='172.18.0.9\n', returncode=0)
            self.assertEqual(helper.resolve_proxy(), 'http://172.18.0.9:7890')
            self.assertEqual(run.call_args[0][0][-1], 'mihomo')


class TestServeRefreshLoop(unittest.TestCase):
    """--serve must keep the state file fresh without cron."""

    def test_refresh_interval_parsing(self):
        with mock.patch.dict(os.environ, {'CODEX_TZ_REFRESH_INTERVAL': '120'}):
            self.assertEqual(helper.refresh_interval(), 120)
        with mock.patch.dict(os.environ, {'CODEX_TZ_REFRESH_INTERVAL': '-5'}):
            self.assertEqual(helper.refresh_interval(), 0)
        with mock.patch.dict(os.environ, {'CODEX_TZ_REFRESH_INTERVAL': 'bogus'}):
            self.assertEqual(helper.refresh_interval(), 900)

    def test_loop_refreshes_initially_and_periodically(self):
        calls = []
        stop = threading.Event()
        with mock.patch.object(helper, 'refresh', side_effect=lambda: calls.append(1)), \
             mock.patch.dict(os.environ, {'CODEX_TZ_REFRESH_INTERVAL': '0.1'}):
            thread = threading.Thread(target=helper.refresh_loop, args=(stop,), daemon=True)
            thread.start()
            time.sleep(0.35)
            stop.set()
            thread.join(timeout=2)
        self.assertGreaterEqual(len(calls), 2)
        self.assertFalse(thread.is_alive())

    def test_loop_stops_after_one_refresh_when_interval_disabled(self):
        calls = []
        stop = threading.Event()
        with mock.patch.object(helper, 'refresh', side_effect=lambda: calls.append(1)), \
             mock.patch.dict(os.environ, {'CODEX_TZ_REFRESH_INTERVAL': '0'}):
            thread = threading.Thread(target=helper.refresh_loop, args=(stop,), daemon=True)
            thread.start()
            thread.join(timeout=2)
        self.assertEqual(len(calls), 1)
        self.assertFalse(thread.is_alive())

    def test_loop_survives_refresh_failures(self):
        calls = []
        def flaky():
            calls.append(1)
            raise RuntimeError('network down')
        stop = threading.Event()
        with mock.patch.object(helper, 'refresh', side_effect=flaky), \
             mock.patch.dict(os.environ, {'CODEX_TZ_REFRESH_INTERVAL': '0.1'}):
            thread = threading.Thread(target=helper.refresh_loop, args=(stop,), daemon=True)
            thread.start()
            time.sleep(0.35)
            stop.set()
            thread.join(timeout=2)
        self.assertGreaterEqual(len(calls), 2)
        self.assertFalse(thread.is_alive())


if __name__ == "__main__":
    unittest.main()
