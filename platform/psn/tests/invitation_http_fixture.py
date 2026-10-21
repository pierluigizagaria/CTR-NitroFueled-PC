# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise fixed-recipient dialog sends through the real backend DLL and HTTP binding."""
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

from bridge_http_fixture import Handler as BaseHandler, Server as BaseServer, SENDER

SESSION = '76561198099999999'


class Server(BaseServer):
    def handle_error(self, *_):
        self.handler_failed = True


class Handler(BaseHandler):
    def web_reply(self, status=0, http=200, body=None):
        raw = b'' if body is None else json.dumps(body).encode()
        self.reply(200, dict(status=status, httpStatus=http,
                             contentType='application/json', bodyHex=raw.hex()))

    def do_POST(self):
        if not self.authorized():
            return
        assert self.path == '/psn/v1/webapi'
        data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        assert set(data) == {'method', 'api_group', 'path', 'content_type', 'headers', 'bodyHex'}
        assert data['headers'] == {} and data['content_type'] == 'application/json'
        mode = self.server.mode
        if data['path'] == '/v1/users/me/friendList':
            assert data['method'] == 'GET' and data['api_group'] == 'friendList' and data['bodyHex'] == ''
            with self.server.lock:
                self.server.lookups += 1
            self.server.marker.write_text('lookup')
            # Keep every response pending long enough to verify copied inputs and
            # asynchronous guest polling. Cancellation is coordinated by marker.
            time.sleep(1.5 if mode == 'cancel-lookup' else 0.05)
            if mode == 'lookup-provider-error':
                self.web_reply(status=-3)
                return
            if mode == 'lookup-http-error':
                self.web_reply(http=503)
                return
            name = '123456789' if mode == 'numeric-name' else 'Friend'
            account = int(SENDER) if mode == 'bad-list' else SENDER
            entries = [dict(user=dict(accountId=account, onlineId=name),
                            presence=dict(onlineStatus='offline' if mode == 'offline' else 'online'))]
            if mode == 'ambiguous':
                entries.append(dict(user=dict(accountId=str(int(SENDER) + 1), onlineId=name),
                                    presence=dict(onlineStatus='online')))
            if mode == 'account':
                # Explicit account selection wins even when two friends share a name.
                entries.append(dict(user=dict(accountId=str(int(SENDER) + 1), onlineId=name),
                                    presence=dict(onlineStatus='online')))
            self.web_reply(body=dict(friendList=entries, start=0, size=len(entries),
                                     totalResults=len(entries) + (mode == 'incomplete-list')))
            return
        assert data['path'] == f'/v1/sessions/{SESSION}/invitations'
        assert data['method'] == 'POST' and data['api_group'] == 'sessionInvitation'
        assert json.loads(bytes.fromhex(data['bodyHex'])) == {'to': [SENDER]}
        with self.server.lock:
            self.server.sends += 1
        self.server.marker.write_text('send')
        if mode == 'cancel-send':
            time.sleep(1.5)
        if mode == 'lost-response':
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        self.web_reply(status=-6 if mode == 'send-provider-error' else 0,
                       http=403 if mode == 'send-http-error' else 204)


def main():
    modes = ('account', 'legacy', 'numeric-name', 'zero-user', 'guest', 'ambiguous', 'unknown',
             'offline', 'bad-list', 'incomplete-list', 'lookup-provider-error', 'lookup-http-error',
             'send-provider-error', 'send-http-error', 'lost-response', 'cancel-lookup', 'cancel-send',
             'invalid')
    with tempfile.TemporaryDirectory(prefix='psn-invitation-fixture-', dir=Path.cwd()) as temporary:
        for mode in modes:
            server = Server(('127.0.0.1', 0), Handler)
            server.mode, server.events, server.replayed, server.failure = mode, [], False, ''
            server.lookups, server.sends, server.handler_failed = 0, 0, False
            server.marker = Path(temporary) / mode
            server.lock = threading.Lock()
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                subprocess.run([sys.argv[1], sys.argv[2], f'http://127.0.0.1:{server.server_port}',
                                mode, str(server.marker)], check=True, timeout=12)
                assert not server.handler_failed
                expected_send = mode in {'account', 'legacy', 'numeric-name', 'zero-user', 'guest',
                                         'send-provider-error', 'send-http-error', 'lost-response', 'cancel-send'}
                assert server.lookups == (mode != 'invalid'), (mode, server.lookups)
                assert server.sends == expected_send, (mode, server.sends)
            finally:
                server.shutdown()
                server.server_close()
                thread.join()
            print('PASS fixed-recipient invitation', mode, flush=True)


if __name__ == '__main__':
    main()
