#!/usr/bin/env python3
"""Server that mixes unknown frame types and header indices into a response."""
import socket, sys
port = int(sys.argv[1]) if len(sys.argv) > 1 else 9555
def frame(t, f, s, p=b""):
    return len(p).to_bytes(3, "big") + bytes([t, f, 0]) + s.to_bytes(2, "big") + p
ls = socket.create_server(("127.0.0.1", port)); ls.settimeout(10)
c, _ = ls.accept()
h = c.recv(8); c.recv(int.from_bytes(h[:3], "big"))
sid = int.from_bytes(h[6:8], "big")
fields = (bytes([0x80 | 4]) + (10).to_bytes(2, "big") + b"text/plain"
          + bytes([0x80 | 99]) + (4).to_bytes(2, "big") + b"v2!!")
c.sendall(frame(0x7F, 0xFF, 0, b"connection-level v2 frame")
          + frame(0x02, 0x00, sid, b"\x00\xc8" + fields)
          + frame(0x50, 0x01, sid, b"stream-level v2 frame")
          + frame(0x03, 0x01, sid, b"skipped cleanly\n"))
c.close()
