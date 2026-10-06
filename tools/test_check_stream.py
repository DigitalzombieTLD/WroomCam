#!/usr/bin/env python3
"""Host-side test: serve frames in the firmware's multipart format and validate them with check_stream."""
import http.server
import threading
import unittest

from check_stream import check_stream

JPEG = b"\xff\xd8" + b"\x00" * 32 + b"\xff\xd9"
BOUNDARY = "wroomcamframe"


def make_handler(frame):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", f"multipart/x-mixed-replace;boundary={BOUNDARY}")
            self.end_headers()
            for n in range(10):
                prefix = b"\r\n" if n else b""
                self.wfile.write(prefix + f"--{BOUNDARY}\r\nContent-Type: image/jpeg\r\n"
                                 f"Content-Length: {len(frame)}\r\n\r\n".encode() + frame)

    return Handler


class CheckStreamTest(unittest.TestCase):
    def run_server(self, frame):
        srv = http.server.HTTPServer(("127.0.0.1", 0), make_handler(frame))
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.shutdown)
        return f"http://127.0.0.1:{srv.server_port}/stream"

    def test_valid_stream(self):
        self.assertTrue(check_stream(self.run_server(JPEG), 5).startswith("OK: 5 frames"))

    def test_non_jpeg_rejected(self):
        with self.assertRaises(AssertionError):
            check_stream(self.run_server(b"notajpeg" * 4), 1)


if __name__ == "__main__":
    unittest.main()
