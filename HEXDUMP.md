# Hexdump

One GET and its response, from:

```
$ ./bserve ./www 9000 &
$ ./bcurl -v -H 'x-trace: 7' localhost:9000/hello.txt
```

`-H 'x-trace: 7'` adds a literal-name field next to the three indexed ones. 68 bytes
request, 149 + 28 bytes response.

## Frame 1: REQUEST (8 + 60 bytes)

```
> 0000  00 00 3c 01 01 00 00 01  01 00 0a 2f 68 65 6c 6c  |..<......../hell|
> 0010  6f 2e 74 78 74 81 00 0e  6c 6f 63 61 6c 68 6f 73  |o.txt...localhos|
> 0020  74 3a 39 30 30 30 82 00  09 62 63 75 72 6c 2f 31  |t:9000...bcurl/1|
> 0030  2e 30 83 00 03 2a 2f 2a  00 07 78 2d 74 72 61 63  |.0...*/*..x-trac|
> 0040  65 00 01 37                                       |e..7|
```

| Offset | Bytes | Field | Meaning |
|---|---|---|---|
| 00 | `00 00 3c` | Length | payload is 60 bytes |
| 03 | `01` | Type | REQUEST |
| 04 | `01` | Flags | END_STREAM (no request body) |
| 05 | `00` | Reserved | always 0 |
| 06 | `00 01` | Stream ID | stream 1 |
| 08 | `01` | Method | GET |
| 09 | `00 0a` | Path length | 10 |
| 0b | `2f 68 … 74` | Path | `/hello.txt` |
| 15 | `81` | Field | index 1, host |
| 16 | `00 0e` | Value length | 14 |
| 18 | `6c 6f … 30` | Value | `localhost:9000` |
| 26 | `82` | Field | index 2, user-agent |
| 27 | `00 09` | Value length | 9 |
| 29 | `62 63 … 30` | Value | `bcurl/1.0` |
| 32 | `83` | Field | index 3, accept |
| 33 | `00 03` | Value length | 3 |
| 35 | `2a 2f 2a` | Value | `*/*` |
| 38 | `00` | Field | literal name |
| 39 | `07` | Name length | 7 |
| 3a | `78 2d … 65` | Name | `x-trace` |
| 41 | `00 01` | Value length | 1 |
| 43 | `37` | Value | `7` |

3 + 10 + 17 + 12 + 6 + 12 = 60.

## Frame 2: RESPONSE (8 + 141 bytes)

```
< 0000  00 00 8d 02 00 00 00 01  00 c8 84 00 19 74 65 78  |.............tex|
< 0010  74 2f 70 6c 61 69 6e 3b  20 63 68 61 72 73 65 74  |t/plain; charset|
< 0020  3d 75 74 66 2d 38 85 00  02 32 30 86 00 1d 54 75  |=utf-8...20...Tu|
< 0030  65 2c 20 30 36 20 4f 63  74 20 32 30 32 36 20 31  |e, 06 Oct 2026 1|
< 0040  33 3a 34 32 3a 33 37 20  47 4d 54 87 00 0d 22 36  |3:42:37 GMT..."6|
< 0050  61 63 34 66 61 63 64 2d  31 34 22 8a 00 0a 6d 61  |ac4facd-14"...ma|
< 0060  78 2d 61 67 65 3d 36 30  88 00 0a 62 73 65 72 76  |x-age=60...bserv|
< 0070  65 2f 31 2e 30 89 00 1d  54 75 65 2c 20 30 36 20  |e/1.0...Tue, 06 |
< 0080  4f 63 74 20 32 30 32 36  20 31 33 3a 34 39 3a 30  |Oct 2026 13:49:0|
< 0090  32 20 47 4d 54                                    |2 GMT|
```

| Offset | Bytes | Field | Meaning |
|---|---|---|---|
| 00 | `00 00 8d` | Length | 141 bytes |
| 03 | `02` | Type | RESPONSE |
| 04 | `00` | Flags | no END_STREAM: DATA follows |
| 05 | `00` | Reserved | 0 |
| 06 | `00 01` | Stream ID | answers stream 1 |
| 08 | `00 c8` | Status | 200 |
| 0a | `84 00 19` + 25 bytes | content-type | `text/plain; charset=utf-8` |
| 26 | `85 00 02` + 2 | content-length | `20` (ASCII decimal) |
| 2b | `86 00 1d` + 29 | last-modified | `Tue, 06 Oct 2026 13:42:37 GMT` |
| 4b | `87 00 0d` + 13 | etag | `"6ac4facd-14"` (mtime-size in hex) |
| 5b | `8a 00 0a` + 10 | cache-control | `max-age=60` |
| 68 | `88 00 0a` + 10 | server | `bserve/1.0` |
| 75 | `89 00 1d` + 29 | date | `Tue, 06 Oct 2026 13:49:02 GMT` |

2 + 28 + 5 + 32 + 16 + 13 + 13 + 32 = 141.

## Frame 3: DATA (8 + 20 bytes)

```
< 0000  00 00 14 03 01 00 00 01  68 65 6c 6c 6f 2c 20 62  |........hello, b|
< 0010  69 6e 61 72 79 20 77 6f  72 6c 64 0a              |inary world.|
```

| Offset | Bytes | Field | Meaning |
|---|---|---|---|
| 00 | `00 00 14` | Length | 20 bytes |
| 03 | `03` | Type | DATA |
| 04 | `01` | Flags | END_STREAM: response complete |
| 05 | `00` | Reserved | 0 |
| 06 | `00 01` | Stream ID | stream 1 |
| 08 | `68 65 … 0a` | Body | `hello, binary world\n` |

20 bytes, matching content-length. Stream 1 is done; the connection stays open.

## Unknown frame

From `tests/client_skip.py`, an unknown type with all flags set:

```
< UNKNOWN(0x7f) stream=0 flags=0xff length=25
< 0000  00 00 19 7f ff 00 00 00  63 6f 6e 6e 65 63 74 69  |........connecti|
< 0010  6f 6e 2d 6c 65 76 65 6c  20 76 32 20 66 72 61 6d  |on-level v2 fram|
< 0020  65                                                |e|
* skipped frame type 0x7f (25 bytes)
```

bcurl discards the 25 bytes and reads the next frame.
