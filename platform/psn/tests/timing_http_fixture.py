# SPDX-License-Identifier: GPL-2.0-or-later
"""Opt-in timing observations on a synthetic loopback service; no Steam/game."""
import http.server
import json
import os
import re
import subprocess
import sys
import threading
import time

TOKEN = 'a' * 64
OWNER, SENDER, SESSION = '76561198000000001', '76561198000000002', '76561198099999999'


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, value):
        raw = json.dumps(value).encode()
        self.send_response(200)
        self.send_header('Content-Length', str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def authorized(self):
        assert self.headers['Authorization'] == 'Bearer ' + TOKEN

    def do_GET(self):
        self.authorized()
        if self.path == '/psn/v1/info':
            self.reply(dict(version=1, accountId=OWNER, onlineId='PrivateOwner',
                            signedIn=True, capabilities=[
                                'identity', 'id-token', 'webapi', 'push', 'invitation-accepted']))
            return
        assert self.path.startswith('/psn/v1/events?after=')
        after = int(self.path.split('=')[1])
        invitation = dict(version=1, kind='invitationAccepted', sessionId=SESSION,
                          invitationId='private-timing-invitation')
        invitation['from'] = dict(accountId=SENDER, onlineId='PrivateSender')
        invitation['to'] = dict(accountId=OWNER, onlineId='PrivateOwner')
        self.reply(dict(version=1, events=[] if after else [
            dict(sequence=1, event=invitation)]))

    def do_POST(self):
        self.authorized()
        assert self.path == '/psn/v1/webapi'
        data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        assert data['api_group'] == 'private-timing-group'
        assert data['headers'] == {'X-Private-Timing': 'private-timing-header'}
        assert bytes.fromhex(data['bodyHex']) == b'private-timing-payload'
        time.sleep(0.20 if '/members?' in data['path'] else 0.05)
        self.reply(dict(status=0, httpStatus=200, contentType='application/octet-stream',
                        bodyHex=b'private-timing-response'.hex()))


def main():
    for setting in (None, '0', 'true', '1'):
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            env = os.environ.copy()
            env.pop('PSN_BRIDGE_TRACE_TIMING', None)
            if setting is not None:
                env['PSN_BRIDGE_TRACE_TIMING'] = setting
            result = subprocess.run(
                [sys.argv[1], sys.argv[2], f'http://127.0.0.1:{server.server_port}', TOKEN],
                env=env, capture_output=True, text=True, timeout=10)
            assert result.returncode == 0, result.stderr
            records = result.stderr.splitlines()
            if setting != '1':
                assert not records, 'Timing emitted without exact opt-in'
                continue
            assert len(records) == 4, 'Unexpected or duplicated timing output'
            forbidden = (TOKEN, OWNER, SENDER, SESSION, 'private-timing', 'PrivateOwner',
                         'PrivateSender', 'X-Private-Timing', '/v1/', 'http://', 'Bearer',
                         b'private-timing-payload'.hex(), b'private-timing-response'.hex())
            assert all(value not in result.stderr for value in forbidden), 'Private data in timing log'
            received = [re.fullmatch(
                r'PSN timing: invitation_received t_ms=(\d+) fetch_ms=(\d+)', line)
                for line in records if 'invitation_received' in line]
            delivered = [re.fullmatch(
                r'PSN timing: invitation_to_host t_ms=(\d+) queue_ms=(\d+)', line)
                for line in records if 'invitation_to_host' in line]
            assert len(received) == len(delivered) == 1 and received[0] and delivered[0]
            assert abs(int(delivered[0][1]) - int(received[0][1]) - int(delivered[0][2])) <= 1
            completed = {}
            for line in records:
                if 'web_complete' not in line:
                    continue
                record = re.fullmatch(
                    r'PSN timing: web_complete t_ms=(\d+) class=([a-z_]+) queue_ms=(\d+) '
                    r'service_ms=(\d+) consume_ms=(\d+) result=0 http=200', line)
                assert record, 'Unexpected completion format'
                completed[record[2]] = tuple(map(int, (record[1], record[3], record[4], record[5])))
            assert set(completed) == {'membership_add', 'session_data'}
            assert completed['membership_add'][2] >= 150, 'Service delay not observed'
            assert completed['membership_add'][3] >= 150, 'Retained-result delay not observed'
            assert completed['session_data'][1] >= 100, 'Worker queue delay not observed'
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
    print('Timing opt-in, queue/service/consumption observations and privacy passed')


if __name__ == '__main__':
    main()
