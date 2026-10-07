#!/usr/bin/env python3
"""Conformance checks for a BHTTP/1 server. usage: conformance.py <host> <port>"""
import socket
import sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9000

REQUEST, RESPONSE, DATA = 0x01, 0x02, 0x03
END_STREAM = 0x01
GET, HEAD = 0x01, 0x02
NAMES = [None, "host", "user-agent", "accept", "content-type", "content-length",
         "last-modified", "etag", "server", "date", "cache-control"]


def frame(ftype, flags, stream, payload=b""):
    return (len(payload).to_bytes(3, "big") + bytes([ftype, flags, 0])
            + stream.to_bytes(2, "big") + payload)


def field(name, value):
    v = value.encode() if isinstance(value, str) else value
    if isinstance(name, int):
        head = bytes([0x80 | name])
    else:
        n = name.encode()
        head = b"\x00" + bytes([len(n)]) + n
    return head + len(v).to_bytes(2, "big") + v


def request(path, method=GET, fields=None):
    p = path.encode()
    if fields is None:
        fields = [field(1, f"{HOST}:{PORT}"), field(2, "conformance/1")]
    return bytes([method]) + len(p).to_bytes(2, "big") + p + b"".join(fields)


def parse_fields(p):
    out, off = {}, 0
    while off < len(p):
        b = p[off]; off += 1
        if b == 0:
            nl = p[off]; off += 1
            name = p[off:off + nl].decode(); off += nl
        else:
            idx = b & 0x7F
            name = NAMES[idx] if idx < len(NAMES) else f"#{idx}"
        vl = int.from_bytes(p[off:off + 2], "big"); off += 2
        out[name] = p[off:off + vl].decode(errors="replace"); off += vl
    return out


class Conn:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=5)

    def send(self, data):
        self.s.sendall(data)

    def exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.s.recv(n - len(buf))
            if not chunk:
                raise EOFError("server closed the connection")
            buf += chunk
        return buf

    def response(self, stream):
        status, headers, body, data_frames = None, {}, b"", 0
        while True:
            h = self.exact(8)
            n = int.from_bytes(h[0:3], "big")
            ftype, flags, sid = h[3], h[4], int.from_bytes(h[6:8], "big")
            payload = self.exact(n)
            if ftype not in (RESPONSE, DATA):
                continue
            assert sid == stream, f"got stream {sid}, wanted {stream}"
            if ftype == RESPONSE:
                status = int.from_bytes(payload[:2], "big")
                headers = parse_fields(payload[2:])
            else:
                data_frames += 1
                body += payload
            if flags & END_STREAM:
                return status, headers, body, data_frames

    def get(self, stream, path, **kw):
        self.send(frame(REQUEST, END_STREAM, stream, request(path, **kw)))
        return self.response(stream)


results = []


def check(name, fn):
    try:
        ok = bool(fn())
    except Exception as e:
        ok, name = False, f"{name}  [{type(e).__name__}: {e}]"
    print(("PASS  " if ok else "FAIL  ") + name)
    results.append(ok)


c = Conn()  # all checks use one connection


def t_get():
    st, h, body, _ = c.get(1, "/hello.txt")
    return st == 200 and int(h["content-length"]) == len(body) and body

def t_keepalive():
    st, *_ = c.get(2, "/")
    return st == 200

def t_unknown_type():
    c.send(frame(0x7F, 0xFF, 0, b"unknown"))
    c.send(frame(0x42, 0x00, 3, b""))
    st, *_ = c.get(3, "/hello.txt")
    return st == 200

def t_malformed():
    bad = bytes([GET]) + (500).to_bytes(2, "big") + b"/short"
    c.send(frame(REQUEST, END_STREAM, 4, bad))
    st, *_ = c.response(4)
    return st == 400

def t_survives_400():
    st, *_ = c.get(5, "/hello.txt")
    return st == 200

def t_404():
    st, *_ = c.get(6, "/definitely-not-here.html")
    return st == 404

def t_traversal():
    st, _, body, _ = c.get(7, "/../../../../../../etc/passwd")
    return st in (403, 404) and b"root:" not in body

def t_head():
    st, h, body, frames = c.get(8, "/hello.txt", method=HEAD)
    return st == 200 and frames == 0 and body == b"" and "content-length" in h

def t_unknown_index():
    st, *_ = c.get(9, "/hello.txt", fields=[field(42, "from the future")])
    return st == 200

def t_literal():
    st, *_ = c.get(10, "/hello.txt", fields=[field("x-trace", "abc123")])
    return st == 200

def t_reserved_byte():
    req = request("/hello.txt", fields=[]) + b"\x05\x00\x00"
    c.send(frame(REQUEST, END_STREAM, 11, req))
    st, *_ = c.response(11)
    return st == 400

def t_unknown_method():
    st, *_ = c.get(12, "/hello.txt", method=0x09)
    return st == 501

def t_unknown_flags():
    c.send(frame(REQUEST, END_STREAM | 0x40, 13, request("/hello.txt")))
    st, *_ = c.response(13)
    return st == 200

def t_oversized():
    junk = request("/hello.txt", fields=[field("x-big", "A" * 60000),
                                         field("x-big2", "B" * 10000)])
    c.send(frame(REQUEST, END_STREAM, 14, junk))
    st, *_ = c.response(14)
    st2, *_ = c.get(15, "/hello.txt")
    return st == 400 and st2 == 200

def t_pipelined():
    c.send(frame(REQUEST, END_STREAM, 16, request("/hello.txt"))
           + frame(REQUEST, END_STREAM, 17, request("/nope")))
    a, *_ = c.response(16)
    b, *_ = c.response(17)
    return a == 200 and b == 404

def t_reserved_set():
    raw = bytearray(frame(REQUEST, END_STREAM, 18, request("/hello.txt")))
    raw[5] = 0xAA
    c.send(bytes(raw))
    st, *_ = c.response(18)
    return st == 200


check("GET /hello.txt -> 200, content-length matches body", t_get)
check("second request on the same connection", t_keepalive)
check("unknown frame types skipped", t_unknown_type)
check("malformed REQUEST -> 400", t_malformed)
check("connection stays open after a 400", t_survives_400)
check("missing file -> 404", t_404)
check("path traversal does not escape the root", t_traversal)
check("HEAD -> RESPONSE with END_STREAM, no DATA", t_head)
check("unknown header index skipped", t_unknown_index)
check("literal header name accepted", t_literal)
check("reserved field-type byte -> 400", t_reserved_byte)
check("unknown method -> 501", t_unknown_method)
check("unknown flag bits ignored", t_unknown_flags)
check("header block > 64 KiB -> 400, connection survives", t_oversized)
check("pipelined requests answered in order", t_pipelined)
check("non-zero reserved byte ignored", t_reserved_set)

print(f"\n{sum(results)}/{len(results)} passed")
sys.exit(0 if all(results) else 1)
