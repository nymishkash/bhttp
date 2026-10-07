# bhttp

BHTTP/1, a binary framing of HTTP. SST Network Architecture course project.

- `bserve`: static file server (track 1)
- `bcurl`: client (track 2)

| Deliverable | File |
|---|---|
| Spec | [SPEC.md](SPEC.md) |
| Programs | [src/bserve.c](src/bserve.c), [src/bcurl.c](src/bcurl.c) |
| Annotated hexdump | [HEXDUMP.md](HEXDUMP.md) |

I did this solo, so both tracks are here. The two programs share no code, and each is
also tested against something other than the other: `tests/conformance.py` is a raw-socket
client that tests the server, and `tests/client_skip.py` is a fake v2 server that tests the
client.

## Build

Needs cc and make. Tests need Python 3.

```sh
make
./bserve ./www 9000 &
./bcurl -v localhost:9000/index.html
```

## bcurl

```
./bcurl [-v] [-I] [-H 'name: value']... URL [URL...]

  URL   [bhttp://]host[:port][/path]     default port 9000
  -v    hexdump frames to stderr ('>' sent, '<' received)
  -I    send HEAD, print status and headers
  -H    add a request header
```

Multiple URLs are fetched over one connection and must have the same host:port.

Exit codes: 0 ok, 1 a response was 4xx/5xx, 2 usage, 3 network/protocol error.

## Tests

```sh
make test
```

`tests/run.sh` runs bcurl against bserve (exit codes, a 300 KB file split over many DATA
frames, empty file, three URLs on one connection), then `client_skip.py`, then
`conformance.py`.

To test another server: `python3 tests/conformance.py <host> <port>`.

## Hexdump

HEXDUMP.md was generated with:

```sh
./bserve ./www 9000 &
./bcurl -v -H 'x-trace: 7' localhost:9000/hello.txt
```
