#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Ismael Luceno <ismael@iodev.co.uk>
# SPDX-License-Identifier: GPL-2.0-or-later
"""test/range_server.py -- scripted HTTP server for the range-resume
regression suite (test/range-http.sh).

Serves a deterministic payload with an ETag and a Last-Modified, honouring
Range and If-Range, with fault modes picked on the command line:

  normal          full Range + If-Range support
  stall:N         answer, send N body bytes, then never answer again
  ignore-range    always 200 the whole body, Range or not
  flaky-range     206 only for an unbounded "bytes=0-", 200 for other ranges
  short-read:N    promise the whole body, close the connection after N bytes
  switch          payload 1 for the first request, payload 2 after that

The payloads are deterministic random.Random() streams, so the suite can
regenerate them byte for byte; payload 2 differs from payload 1 in seed,
length, ETag and date, so a file spliced from the two can never pass a
content comparison.

Every request is appended to the log file as one line:
  RANGE=<range or -> IF-RANGE=<validator or -> -> <status>

Usage: range_server.py --mode MODE --port-file FILE --log FILE
                       [--payload 1|2]
The actual port is written to the port file once the server is listening.
"""

import argparse
import http.server
import random
import threading

# payload number -> (seed, length, etag, last-modified)
PAYLOADS = {
    1: (1, 600000, '"alpha"', 'Sun, 06 Nov 1994 08:49:37 GMT'),
    2: (2, 450123, '"bravo"', 'Mon, 07 Nov 1994 08:49:37 GMT'),
}


def make_payload(number):
    seed, length, _, _ = PAYLOADS[number]
    return random.Random(seed).randbytes(length)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'

    def log_message(self, *args):
        pass

    def _log(self, status):
        range_ = self.headers.get('Range') or '-'
        if_range = self.headers.get('If-Range') or '-'
        with self.server.log_lock:
            with open(self.server.log_path, 'a') as f:
                f.write('RANGE=%s IF-RANGE=%s -> %d\n'
                        % (range_, if_range, status))

    def _send(self, status, body, extra=(), length=None):
        self.send_response(status)
        for name, value in extra:
            self.send_header(name, value)
        # The promised length may exceed what is actually written: the
        # stall and short-read faults are exactly that lie.
        self.send_header('Content-Length',
                         str(len(body) if length is None else length))
        self.end_headers()
        self._log(status)
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # the client only wanted the headers

    def _current(self):
        """(payload number, body, etag, last-modified) for this request."""
        number = self.server.payload
        if self.server.mode == 'switch':
            with self.server.lock:
                self.server.requests += 1
                if self.server.requests > 1:
                    number = 2
        seed, _, etag, lastmod = PAYLOADS[number]
        return number, self.server.bodies[number], etag, lastmod

    def do_GET(self):
        mode = self.server.mode
        _, body, etag, lastmod = self._current()
        range_ = self.headers.get('Range')
        if_range = self.headers.get('If-Range')

        if mode == 'ignore-range' or mode.startswith('short-read'):
            range_ = None

        if mode == 'flaky-range' and range_ != 'bytes=0-':
            range_ = None

        # If-Range: only a matching validator keeps the request conditional
        if range_ and if_range and if_range not in (etag, lastmod):
            range_ = None

        if not range_:
            if mode.startswith('short-read'):
                cut = int(mode.split(':')[1])
                self._send(200, body[:cut], length=len(body))
                return
            self._send(200, body, [('ETag', etag),
                                   ('Last-Modified', lastmod)])
            return

        spec = range_.partition('=')[2]
        start_s, _, end_s = spec.partition('-')
        start = int(start_s)
        end = int(end_s) if end_s else len(body) - 1
        chunk = body[start:end + 1]
        headers = [('Content-Range', 'bytes %d-%d/%d'
                    % (start, end, len(body))),
                   ('ETag', etag),
                   ('Last-Modified', lastmod)]
        if mode.startswith('stall:'):
            cut = int(mode.split(':')[1])
            self._send(206, chunk[:cut], headers, length=len(chunk))
            threading.Event().wait()  # never answer again
            return
        self._send(206, chunk, headers)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='normal')
    ap.add_argument('--payload', type=int, default=1)
    ap.add_argument('--port-file', required=True)
    ap.add_argument('--log', required=True)
    args = ap.parse_args()

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    server.mode = args.mode
    server.payload = args.payload
    server.bodies = {n: make_payload(n) for n in PAYLOADS}
    server.requests = 0
    server.lock = threading.Lock()
    server.log_lock = threading.Lock()
    server.log_path = args.log

    with open(args.port_file, 'w') as f:
        f.write(str(server.server_address[1]))
    server.serve_forever()


if __name__ == '__main__':
    main()
