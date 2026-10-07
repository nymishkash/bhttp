#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
make -s || exit 1

# no timeout(1) on stock macOS
command -v timeout >/dev/null 2>&1 || timeout() { shift; "$@"; }

PORT=${PORT:-9137}
ROOT=$(mktemp -d)
LOG=$(mktemp)
cp -R www/. "$ROOT"
head -c 300000 /dev/urandom > "$ROOT/big.bin"
: > "$ROOT/empty.txt"

./bserve "$ROOT" "$PORT" >/dev/null 2>"$LOG" &
SRV=$!
trap 'kill $SRV 2>/dev/null; rm -rf "$ROOT" "$LOG"' EXIT
sleep 0.3

fail=0
pass() { echo "PASS  $*"; }
bad()  { echo "FAIL  $*"; fail=1; }

expect() {
  local want=$1; shift
  timeout 10 "$@" >/dev/null 2>&1
  local got=$?
  [ "$got" = "$want" ] && pass "exit $got: ${*#./}" || bad "exit $got, wanted $want: $*"
}

U=localhost:$PORT
echo "== bcurl against bserve"
expect 0 ./bcurl $U/index.html
expect 0 ./bcurl $U/
expect 0 ./bcurl -I $U/hello.txt
expect 1 ./bcurl $U/missing.html
expect 1 ./bcurl $U/hello.txt $U/missing.html
expect 2 ./bcurl $U/a otherhost:$PORT/b
expect 3 ./bcurl localhost:1/index.html

timeout 10 ./bcurl $U/big.bin > /tmp/big.out 2>/dev/null
cmp -s /tmp/big.out "$ROOT/big.bin" && pass "300 KB body" || bad "big.bin corrupted"
[ "$(timeout 10 ./bcurl $U/empty.txt | wc -c)" -eq 0 ] && pass "empty file" || bad "empty file"

timeout 10 ./bcurl $U/hello.txt $U/index.html $U/docs/ >/dev/null 2>&1
conns=$(tail -n 3 "$LOG" | sed 's/.*conn=\([0-9]*\).*/\1/' | sort -u | wc -l)
[ "$conns" -eq 1 ] && pass "3 URLs, 1 connection" || bad "bcurl opened $conns connections"

(python3 tests/client_skip.py 9555 &) ; sleep 0.5
[ "$(timeout 5 ./bcurl localhost:9555/x 2>/dev/null)" = "skipped cleanly" ] \
  && pass "client skips unknown frames" || bad "client skips unknown frames"

echo; echo "== conformance"
python3 tests/conformance.py 127.0.0.1 "$PORT" || fail=1

echo; [ $fail -eq 0 ] && echo "ok" || echo "FAILED"
exit $fail
