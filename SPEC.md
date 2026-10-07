# BHTTP/1

Nymish, SST Network Architecture. (Unrelated to RFC 9292.)

The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119. All integers are
unsigned and big-endian (network byte order). Byte offsets count from 0.

## 1. Model

A client opens one TCP connection to a server and sends REQUEST frames on it. The server
answers each with one RESPONSE frame followed by zero or more DATA frames. The connection
stays open after every response, including error responses, until a peer closes it. A client
needing several resources from the same server sends them all on that one connection.

## 2. Frame header (8 bytes, fixed)

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
+-----------------------------------------------+---------------+
|                  Length (24)                  |   Type (8)    |
+---------------+---------------+---------------+---------------+
|   Flags (8)   | Reserved (8)  |        Stream ID (16)         |
+---------------+---------------+-------------------------------+
|                 Payload (Length bytes) ...
```

| Field | Bytes | Meaning |
|---|---|---|
| Length | 0-2 | Payload size in bytes, 0 to 16,777,215. Excludes these 8 bytes. |
| Type | 3 | What the payload is (§3). |
| Flags | 4 | Bit field; meaning depends on Type. |
| Reserved | 5 | Senders MUST send 0x00. Receivers MUST ignore it. |
| Stream ID | 6-7 | Which request/response exchange this frame belongs to. 0 = the connection itself. |

### Field widths

- Length is first so a receiver can always find the next frame after reading 8 bytes,
  regardless of type (needed for §8). 24 bits allows 16 MiB, which is plenty per frame and
  small enough to reject absurd allocations. Larger bodies are split into DATA frames.
- Type and Flags: 8 bits each. v1 uses 3 types and 1 flag.
- Reserved: 1 byte for v2 (priority, version, etc.). Also keeps the header at 8 bytes.
- Stream ID: 16 bits. v1 is one request at a time, so this only matches responses to
  pipelined requests, but having it in the header lets v2 multiplex without a format change.
  IDs can be reused after a stream ends.

### Comparison with HTTP/2 (24/8/8/31)

HTTP/2 also uses a 24-bit length, with a default max frame size of 16 KiB. Its stream ID is 31
bits because streams are multiplexed and IDs are never reused on a connection (odd for client,
even for server push). The remaining bit is reserved. That makes a 9-byte header; ours is 8
since 16-bit IDs are enough.

## 3. Frame types and flags

| Type | Name | Direction | Payload |
|---|---|---|---|
| 0x01 | REQUEST | client → server | §4 |
| 0x02 | RESPONSE | server → client | §4 |
| 0x03 | DATA | server → client | raw body bytes |
| 0x00, 0x04-0xFF | reserved | - | skipped (§8) |

END_STREAM (0x01) is defined on all three types and means the sender is done with the
stream. A REQUEST always sets it (v1 requests have no body). All other flag bits are unassigned:
senders MUST set them to 0, receivers MUST ignore them.

## 4. REQUEST and RESPONSE payloads

```
REQUEST:   | Method (8) | Path length (16) | Path (bytes) | Header fields ... |
RESPONSE:  | Status (16)                                  | Header fields ... |
```

Method: 0x01 GET, 0x02 HEAD. Path: 1-65,535 bytes, MUST start with `/`, MUST NOT contain
0x00, not percent-encoded. Status: HTTP status code (200 = `00 C8`). Header fields run to the
end of the payload; there is no count.

## 5. Header fields

The first byte says how the name is encoded. The value is always a 16-bit length plus bytes.

```
Indexed name:  | 1 | index (7) | Value length (16) | Value |
Literal name:  | 0x00 | Name length (8) | Name | Value length (16) | Value |
```

A first byte of 0x01-0x7F, or 0x80 (index 0), is malformed. Literal names are 1-255 bytes of
lowercase ASCII. Values are opaque bytes; numbers such as content-length are ASCII decimal,
dates use the HTTP date format. Static table:

| # | name | # | name |
|---|---|---|---|
| 1 | host | 6 | last-modified |
| 2 | user-agent | 7 | etag |
| 3 | accept | 8 | server |
| 4 | content-type | 9 | date |
| 5 | content-length | 10 | cache-control |

Indices 11-127 are reserved. A receiver MUST skip a field with an unknown index and MUST NOT
reject the frame. This is HPACK's static table and length-prefixed literals, without Huffman
coding or a dynamic table.

## 6. Exchange rules

1. The client picks a non-zero Stream ID per request and MUST NOT reuse it while that stream is
   still open. bcurl uses 1, 2, 3, ...
2. The server answers REQUESTs in the order received, each with exactly one RESPONSE then
   zero or more DATA frames on the same Stream ID. The last frame of the response carries
   END_STREAM. A client MAY send the next REQUEST before the previous response ends
   (pipelining).
3. The body is the concatenation of the DATA payloads. The server sends content-length; it
   SHOULD equal the body size. Servers SHOULD keep DATA payloads at 16,384 bytes or less.
4. HEAD: the RESPONSE carries the headers GET would, sets END_STREAM, and no DATA follows.
   An empty file is likewise a RESPONSE with END_STREAM and `content-length: 0`.
5. Either peer MAY close the connection when it has no stream open. bserve closes after 60 s idle.

## 7. Errors

| Status | When |
|---|---|
| 400 | REQUEST payload malformed: path length runs past the payload, empty path, path not starting `/` or containing 0x00, a field running past the payload, a reserved first byte, a zero-length literal name, or a REQUEST payload larger than 64 KiB. |
| 403 | The resolved path is outside the root. |
| 404 | No regular file at that path. |
| 501 | Unknown method. |

Error responses carry a short `text/plain` body. A malformed REQUEST does not close the
connection: the server has already read Length bytes, so it replies 400 on that stream and
reads the next frame.

## 8. Extensibility

A receiver meeting a frame Type it does not know MUST read and discard exactly Length payload
bytes and continue. It MUST NOT treat this as an error, close the connection, or act on that
frame's flags (END_STREAM on an unknown frame ends nothing). Together with the rules for unknown
flags, the reserved byte and unknown header indices, this lets v1 receivers ignore v2
additions. Stream 0 is reserved for future connection-level frames; v1 defines none.

## 9. Mapping paths to files (server)

The path is appended to the root directory. A path ending in `/` gets `index.html` appended. A
directory is served as its `index.html`. The fully resolved path (following `..` and symlinks)
MUST lie inside the root, or the server answers 403. content-type comes from the file extension,
defaulting to `application/octet-stream`.
