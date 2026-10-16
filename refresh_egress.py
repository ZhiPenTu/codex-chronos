#!/usr/bin/env python3
"""Refresh the timezone state file from the real egress IP.

Container deployments usually sit next to a mihomo sidecar on cpa-proxy-net.
Desktop deployments (macOS/Windows/Linux without Docker) auto-detect a local
proxy (Clash/mihomo/V2Ray style ports) and fall back to a direct connection,
so no container and no mihomo sidecar are required. Never queries OpenAI or
reads OAuth credentials. Atomic writes, last-good-state semantics.

Path resolution mirrors the C plugin (paths.inc):
  CODEX_TZ_STATE / CODEX_TZ_REFRESH_SOCKET  >  CODEX_TZ_DATA_DIR  >
  /CLIProxyAPI/data (when it exists)  >  per-user fallback directory
(macOS ~/.codex-chronos, Linux ${XDG_DATA_HOME:-~/.local/share}/codex-chronos,
Windows %LOCALAPPDATA%\\codex-chronos).
"""
import datetime as dt
import ipaddress
import json
import os
import pathlib
import re
import socket
import socketserver
import stat
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from zoneinfo import ZoneInfo

CONTAINER_DIR = '/CLIProxyAPI/data'
DIR_NAME = 'codex-chronos'
URL = 'https://ipinfo.io/json'

# Well-known local HTTP proxy ports (Clash/mihomo/Clash Verge/V2Ray/Surge).
# Surge for Mac listens on 6152 (HTTP); its 6153 is SOCKS-only and excluded.
LOCAL_PROXY_PORTS = (7890, 7897, 7891, 1087, 8118, 8080, 6152)


def data_dir():
    """Resolve the plugin data directory exactly like the C plugin."""
    override = os.environ.get('CODEX_TZ_DATA_DIR')
    if override:
        return pathlib.Path(override)
    container = pathlib.Path(CONTAINER_DIR)
    if container.is_dir():
        return container
    if os.name == 'nt':
        base = (os.environ.get('LOCALAPPDATA') or os.environ.get('APPDATA')
                or os.environ.get('USERPROFILE') or os.path.expanduser('~'))
        return pathlib.Path(base) / DIR_NAME
    if sys.platform == 'darwin':
        return pathlib.Path.home() / f'.{DIR_NAME}'
    xdg = os.environ.get('XDG_DATA_HOME')
    if xdg:
        return pathlib.Path(xdg) / DIR_NAME
    return pathlib.Path.home() / '.local' / 'share' / DIR_NAME


def state_path():
    override = os.environ.get('CODEX_TZ_STATE')
    if override:
        return pathlib.Path(override)
    return data_dir() / 'codex-chronos.state'


def refresh_socket_path():
    override = os.environ.get('CODEX_TZ_REFRESH_SOCKET')
    if override:
        return pathlib.Path(override)
    return data_dir() / 'codex-chronos-refresh.sock'


STATE = state_path()


def _port_open(port):
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=1.5):
            return True
    except OSError:
        return False


def _local_proxy():
    for port in LOCAL_PROXY_PORTS:
        if _port_open(port):
            return f'http://127.0.0.1:{port}'
    return None


def resolve_proxy():
    """Pick the egress proxy.

    Order: CODEX_TZ_PROXY ("direct"/"none" forces a direct connection) >
    HTTPS_PROXY/https_proxy environment > mihomo sidecar via docker inspect >
    first open local proxy port > direct connection.
    """
    override = os.environ.get('CODEX_TZ_PROXY')
    if override is not None:
        if override.strip().lower() in ('', 'direct', 'none', 'off'):
            return None
        return override.strip()
    system = os.environ.get('HTTPS_PROXY') or os.environ.get('https_proxy')
    if system:
        return system
    try:
        result = subprocess.run(
            ['docker', 'inspect', '-f',
             '{{(index .NetworkSettings.Networks "cpa-proxy-net").IPAddress}}',
             'mihomo'], capture_output=True, text=True, timeout=8, check=True)
        ip = str(ipaddress.ip_address(result.stdout.strip()))
        return f'http://{ip}:7890'
    except Exception:
        pass
    local = _local_proxy()
    if local:
        print(f'egress proxy: detected local {local} '
              f'(set CODEX_TZ_PROXY to override, "direct" to bypass)', flush=True)
        return local
    print('egress proxy: none detected, using direct connection '
          '(set CODEX_TZ_PROXY to override)', flush=True)
    return None


def refresh():
    proxy = resolve_proxy()
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({'https': proxy} if proxy else {}))
    with opener.open(urllib.request.Request(URL, headers={'User-Agent': 'codex-chronos/0.6.0'}),
                     timeout=12) as resp:
        data = json.load(resp)
    ip = str(ipaddress.ip_address(data['ip']))
    zone = str(data['timezone'])
    if not re.fullmatch(r'[A-Za-z0-9_+/-]{1,79}', zone):
        raise ValueError('invalid IANA timezone')
    now = dt.datetime.now(ZoneInfo(zone))
    abbr = now.tzname() or 'UTC'
    offset = now.strftime('%z')
    offset = offset[:3] + ':' + offset[3:] if len(offset) == 5 else '+00:00'
    payload = f'{zone}|{now:%Y-%m-%d}|{abbr}|{offset}\n{ip}\n'
    STATE.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix='.tz-tmp-', dir=STATE.parent)
    try:
        with os.fdopen(fd, 'w', encoding='ascii') as f:
            f.write(payload)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o644)
        os.replace(tmp, STATE)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    via = f' via {proxy}' if proxy else ''
    print(f'{zone} {now:%Y-%m-%d} {abbr} UTC{offset} {ip}{via}')


_lock = threading.Lock()


class RefreshHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(16)
        try:
            command = self.request.recv(32)
            if command != b'REFRESH\n':
                self.request.sendall(b'ERR\n')
                return
            with _lock:
                refresh()
            self.request.sendall(b'OK\n')
        except Exception as exc:  # reply ERR with a short reason, never crash
            print(f'refresh helper error: {type(exc).__name__}: {exc}', file=sys.stderr)
            try:
                self.request.sendall(f'ERR {type(exc).__name__}\n'.encode('ascii', 'replace')[:220])
            except OSError:
                pass


def windows_poll_once(socket_path):
    """One Windows local-file mailbox iteration; only a trigger ID is exchanged.

    Windows Python does not expose AF_UNIX. Keep the mailbox inside the
    administrator-controlled CPA data directory, protected by Windows ACLs.
    """
    request = pathlib.Path(str(socket_path) + '.request')
    if not request.is_file():
        return False
    try:
        nonce = request.read_text(encoding='ascii').strip()
    except (OSError, UnicodeError):
        nonce = ''
    request.unlink(missing_ok=True)
    if not re.fullmatch(r'[0-9a-f]{16}', nonce):
        return True
    target = pathlib.Path(str(socket_path) + '.' + nonce + '.reply')
    try:
        with _lock:
            refresh()
        result = 'OK\n'
    except Exception as exc:
        print(f'refresh helper error: {type(exc).__name__}: {exc}', file=sys.stderr)
        result = f'ERR {type(exc).__name__}\n'
    fd, temp = tempfile.mkstemp(prefix='.tz-reply-', dir=socket_path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='ascii') as stream:
            stream.write(result)
            stream.flush()
        os.replace(temp, target)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)
    return True


def refresh_interval():
    raw = os.environ.get('CODEX_TZ_REFRESH_INTERVAL', '900')
    try:
        return max(float(raw), 0.0)
    except (TypeError, ValueError):
        print(f'ignoring invalid CODEX_TZ_REFRESH_INTERVAL={raw!r}; using 900',
              file=sys.stderr)
        return 900.0


def refresh_loop(stop=None):
    """Keep the state file fresh without cron: refresh now, then periodically."""
    interval = refresh_interval()
    while True:
        try:
            with _lock:
                refresh()
        except Exception as exc:
            print(f'refresh failed: {type(exc).__name__}: {exc}', file=sys.stderr)
        if interval <= 0:
            return
        if stop is not None:
            if stop.wait(interval):
                return
        else:
            time.sleep(interval)


def serve():
    socket_path = refresh_socket_path()
    if os.name == 'nt':
        return windows_serve(socket_path)
    socket_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        if stat.S_ISSOCK(socket_path.lstat().st_mode):
            socket_path.unlink()
        else:
            raise RuntimeError('refresh socket path exists and is not a socket')
    except FileNotFoundError:
        pass
    old_umask = os.umask(0o077)
    try:
        with socketserver.ThreadingUnixStreamServer(str(socket_path), RefreshHandler) as server:
            os.chmod(socket_path, 0o600)
            print(f'Codex Chronos refresh helper listening on {socket_path}', flush=True)
            print(f'state file: {STATE}', flush=True)
            stop = threading.Event()
            threading.Thread(target=refresh_loop, args=(stop,), name='tz-refresh-loop',
                             daemon=True).start()
            try:
                server.serve_forever(poll_interval=.5)
            finally:
                stop.set()
    finally:
        os.umask(old_umask)
        if socket_path.exists() and stat.S_ISSOCK(socket_path.lstat().st_mode):
            socket_path.unlink()


def windows_serve(socket_path):
    socket_path.parent.mkdir(parents=True, exist_ok=True)
    ready = pathlib.Path(str(socket_path) + '.ready')
    print(f'Codex Chronos Windows helper polling {socket_path}', flush=True)
    print(f'state file: {STATE}', flush=True)
    stop = threading.Event()
    threading.Thread(target=refresh_loop, args=(stop,), name='tz-refresh-loop',
                     daemon=True).start()
    try:
        while True:
            ready.touch()
            windows_poll_once(socket_path)
            threading.Event().wait(.2)
    finally:
        stop.set()
        ready.unlink(missing_ok=True)


if __name__ == '__main__':
    try:
        if sys.argv[1:] == ['--serve']:
            serve()
        elif not sys.argv[1:]:
            refresh()
        else:
            raise ValueError('usage: refresh_egress.py [--serve]')
    except KeyboardInterrupt:
        # Ctrl-C: serve()'s finally block already removed the socket.
        sys.exit(130)
    except Exception as exc:
        print(f'timezone refresh failed: {type(exc).__name__}: {exc}', file=sys.stderr)
        sys.exit(1)
