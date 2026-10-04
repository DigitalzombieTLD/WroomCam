#!/usr/bin/env python3
"""Validate the WroomCam MJPEG stream: multipart framing, Content-Length and JPEG markers.

Usage: tools/check_stream.py http://<device-ip>/stream [frames]
"""
import re
import sys
import time
import urllib.request


def main():
    url = sys.argv[1]
    want = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    resp = urllib.request.urlopen(url, timeout=10)
    ctype = resp.headers.get("Content-Type", "")
    m = re.match(r"multipart/x-mixed-replace;\s*boundary=(\S+)", ctype)
    assert m, f"unexpected Content-Type: {ctype}"
    boundary = b"--" + m.group(1).encode()
    start = time.time()
    total = 0
    for n in range(1, want + 1):
        line = resp.readline()
        while line.strip() == b"":
            line = resp.readline()
        assert line.strip() == boundary, f"frame {n}: bad boundary {line!r}"
        headers = {}
        while True:
            line = resp.readline().strip()
            if not line:
                break
            k, v = line.decode().split(":", 1)
            headers[k.strip().lower()] = v.strip()
        assert headers.get("content-type") == "image/jpeg", f"frame {n}: bad type {headers}"
        length = int(headers["content-length"])
        data = resp.read(length)
        assert len(data) == length, f"frame {n}: short body"
        assert data[:2] == b"\xff\xd8", f"frame {n}: missing JPEG SOI"
        assert b"\xff\xd9" in data[-16:], f"frame {n}: missing JPEG EOI"
        total += length
    dt = time.time() - start
    print(f"OK: {want} frames, {want / dt:.1f} fps, {total / dt / 1024:.0f} KiB/s, avg {total // want} bytes/frame")


if __name__ == "__main__":
    main()
