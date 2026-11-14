#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 Ismael Luceno <ismael@iodev.co.uk>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# test/range-http.sh -- end-to-end resume robustness against a local HTTP
# server that misbehaves on purpose.
#
# The failure these tests pin down: the publisher replaces a file between
# or during runs, and the client keeps writing at the offsets the old file
# gave it, splicing two versions into one corrupt download.  The server is
# test/range_server.py, the faults it can inject are the ones the resume
# logic has to survive: replacement between runs, replacement mid-download,
# Range answered with 200, and a connection cut short.
#
# Every run also checks what is left on disk: the final name is published
# only once the whole representation is there, so an interrupted or
# short-read download must leave a pre-existing file at that name alone.
#
# One test per scenario, in the order a download goes through them.  TAP
# goes to stderr, as test/harness.h does, so tap-run treats this like any
# other suite.  Exit 0 when every scenario behaved, 1 otherwise.
#
# Usage:  range-http.sh
# Env:    SRCTOP    where the source tree is (default: .)
#         AXEL_BIN  the built axel (default: $SRCTOP/axel)
#         PYTHON    python3 interpreter for the test server

set -u

pfx='range http:'
src=${SRCTOP:-.}
axel=${AXEL_BIN:-$src/axel}
server=${SERVER:-$src/test/range_server.py}
python=${PYTHON:-python3}

if [ ! -x "$axel" ] || [ ! -f "$server" ] || ! command -v "$python" >/dev/null; then
	printf '\n1..1\n' >&2
	printf 'ok 1 %s needs an axel binary, python3 and %s # SKIP\n' \
	    "$pfx" "$server" >&2
	exit 0
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/axel-range-http.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
trap 'rm -rf "$tmp"; exit 1' HUP INT TERM

n=0
failed=0

ok() {
	n=$((n + 1))
	if [ "$1" -eq 0 ]; then
		printf 'ok %d %s %s\n' "$n" "$pfx" "$2" >&2
	else
		printf 'not ok %d %s %s\n' "$n" "$pfx" "$2" >&2
		failed=$((failed + 1))
	fi
}

srv_pid=
start_server() {  # mode [payload]
	"$python" "$server" --mode "$1" --payload "${2:-1}" \
	    --port-file "$tmp/port" --log "$tmp/server.log" &
	srv_pid=$!
	i=0
	while [ ! -s "$tmp/port" ] && [ $i -lt 100 ]; do
		sleep 0.1
		i=$((i + 1))
	done
	port=$(cat "$tmp/port" 2>/dev/null || echo 0)
	url="http://127.0.0.1:$port/file"
}

stop_server() {
	kill "$srv_pid" 2>/dev/null
	wait "$srv_pid" 2>/dev/null
	srv_pid=
	rm -f "$tmp/port" "$tmp/server.log"
}

# run_axel DIR [args...] -- run axel in DIR with no proxies and no user config
run_axel() {
	dir=$1
	shift
	(cd "$dir" && env -u http_proxy -u https_proxy \
	    -u HTTP_PROXY -u HTTPS_PROXY -u all_proxy -u ALL_PROXY \
	    HOME="$tmp" "$axel" -q -N "$@" >axel.log 2>&1)
}

# run_axel_timeout SECONDS DIR [args...] -- run_axel with a wall-clock limit;
# timeout(1) cannot exec a shell function, so it wraps the binary itself
run_axel_timeout() {
	secs=$1
	dir=$2
	shift 2
	(cd "$dir" && env -u http_proxy -u https_proxy \
	    -u HTTP_PROXY -u HTTPS_PROXY -u all_proxy -u ALL_PROXY \
	    HOME="$tmp" timeout "$secs" \
	    "$axel" -q -N "$@" >axel.log 2>&1)
}

# payload SEED SIZE OUT -- regenerate the server's deterministic payload
payload() {
	"$python" -c '
import random, sys
open(sys.argv[3], "wb").write(random.Random(int(sys.argv[1])).randbytes(int(sys.argv[2])))
' "$1" "$2" "$3"
}

# wait_for PATH -- wait up to ten seconds for PATH to exist and be non-empty
wait_for() {
	i=0
	while [ $((i += 1)) -lt $((2 * 10)) ]; do
		[ -s "$1" ] && return 0
		sleep 0.5
	done
	return 1
}

printf '\n1..9\n' >&2

# ── 1. a complete download is published under the final name ─────────────
mkdir "$tmp/t1"
start_server normal
run_axel "$tmp/t1" -n 4 "$url"
rc=$?
payload 1 600000 "$tmp/t1.expected"
stop_server
[ $rc -eq 0 ] && cmp -s "$tmp/t1/file" "$tmp/t1.expected" &&
    [ ! -e "$tmp/t1/file.st" ] && [ ! -e "$tmp/t1/file.part" ]
ok $? "a complete download publishes the final file and drops the state"

# ── 2. an interrupted download leaves state, not a final file ────────────
mkdir "$tmp/t2"
start_server stall:65536
(cd "$tmp/t2" && env -u http_proxy -u https_proxy \
    -u HTTP_PROXY -u HTTPS_PROXY HOME="$tmp" \
    "$axel" -q -N -n 4 "$url" >axel.log 2>&1) &
axel_pid=$!
wait_for "$tmp/t2/file.part" || :
sleep 1
kill -INT "$axel_pid" 2>/dev/null
wait "$axel_pid" 2>/dev/null
[ ! -e "$tmp/t2/file" ] && [ -s "$tmp/t2/file.part" ] &&
    [ -s "$tmp/t2/file.st" ] &&
    [ "$(head -c 4 "$tmp/t2/file.st")" = AXST ]
ok $? "an interrupted download leaves part and state, no final file"

# ── 3. cancelling a resume does not touch the file at the final name ─────
printf SENTINEL > "$tmp/t2/file"
before=$(stat -c %s "$tmp/t2/file.part")
(cd "$tmp/t2" && env -u http_proxy -u https_proxy \
    -u HTTP_PROXY -u HTTPS_PROXY HOME="$tmp" \
    "$axel" -q -N -n 4 "$url" >axel.log 2>&1) &
axel_pid=$!
i=0
while [ "$(stat -c %s "$tmp/t2/file.part" 2>/dev/null || echo 0)" = "$before" ] &&
      [ $((i += 1)) -lt 20 ]; do
	sleep 0.5
done
kill -INT "$axel_pid" 2>/dev/null
wait "$axel_pid" 2>/dev/null
stop_server
[ "$(cat "$tmp/t2/file")" = SENTINEL ]
ok $? "cancelling a resume leaves the file at the final name alone"

# ── 4. the file was replaced between runs: old progress is discarded ─────
start_server normal 2
run_axel "$tmp/t2" -n 4 "$url"
rc=$?
payload 2 450123 "$tmp/t2.expected"
stop_server
[ $rc -eq 0 ] && cmp -s "$tmp/t2/file" "$tmp/t2.expected" &&
    [ ! -e "$tmp/t2/file.st" ] && [ ! -e "$tmp/t2/file.part" ]
ok $? "a replaced file discards the old progress and downloads whole"

# ── 5. the file is replaced mid-download: the new one is what lands ──────
mkdir "$tmp/t5"
start_server switch
run_axel "$tmp/t5" -n 4 "$url"
rc=$?
payload 2 450123 "$tmp/t5.expected"
grep -q 'IF-RANGE="alpha"' "$tmp/server.log"
saw_if_range=$?
stop_server
[ $rc -eq 0 ] && cmp -s "$tmp/t5/file" "$tmp/t5.expected" &&
    [ $saw_if_range -eq 0 ]
ok $? "a file replaced mid-download is restarted and fetched whole"

# ── 6. a server without ranges still downloads, in one piece ─────────────
mkdir "$tmp/t6"
start_server ignore-range
run_axel "$tmp/t6" -n 4 "$url"
rc=$?
payload 1 600000 "$tmp/t6.expected"
stop_server
[ $rc -eq 0 ] && cmp -s "$tmp/t6/file" "$tmp/t6.expected"
ok $? "a server ignoring Range falls back to one whole-file download"

# ── 7. a 200 to a ranged request cannot splice the file ──────────────────
mkdir "$tmp/t7"
start_server flaky-range
run_axel_timeout 60 "$tmp/t7" -n 4 "$url"
rc=$?
stop_server
[ $rc -ne 0 ] && [ $rc -ne 124 ] && [ ! -e "$tmp/t7/file" ]
ok $? "a 200 to a ranged request fails the download, publishes nothing"

# ── 8. a short read is not a complete file ───────────────────────────────
mkdir "$tmp/t8"
start_server short-read:65536
run_axel_timeout 60 "$tmp/t8" -n 4 "$url"
rc=$?
stop_server
[ $rc -ne 0 ] && [ $rc -ne 124 ] && [ ! -e "$tmp/t8/file" ]
ok $? "a short read fails the download, publishes nothing"

# ── 9. a state file from before the format is discarded, not resumed ─────
mkdir "$tmp/t9"
"$python" -c '
import struct, sys
open(sys.argv[1], "wb").write(b"stale" * 20)
with open(sys.argv[1] + ".st", "wb") as f:
    f.write(struct.pack("<H", 1))                    # one connection
    f.write(struct.pack("<q", 100))                  # bytes done
    f.write(struct.pack("<qq", 0, 600000))           # the range
' "$tmp/t9/file"
start_server normal
run_axel "$tmp/t9" -n 4 "$url"
rc=$?
payload 1 600000 "$tmp/t9.expected"
stop_server
[ $rc -eq 0 ] && cmp -s "$tmp/t9/file" "$tmp/t9.expected"
ok $? "a legacy state file degrades to a fresh, correct download"

printf '# %s %d of %d scenarios behaved\n' "$pfx" $((9 - failed)) 9 >&2
exit $((failed > 0))
