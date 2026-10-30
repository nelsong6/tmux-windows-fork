#!/bin/sh

# A command client reads and writes files for the server: source-file,
# load-buffer and save-buffer from a second client finish, keep the file's
# bytes, and report errors with exit 1. On Windows they used to wait forever
# (libevent can't watch a file descriptor that isn't a socket).

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
. "$(dirname "$0")/win32-wait.sh"
$TMUX kill-server 2>/dev/null
wait_gone

OUT=$(mktemp -d)
trap "$TMUX kill-server 2>/dev/null; rm -rf $OUT" 0 1 15

fail() {
	echo "FAIL: $*"
	exit 1
}

# run COMMAND...: a tmux command that must finish within 10 s.
run() {
	timeout 10 $TMUX "$@"
	_rc=$?
	[ $_rc -eq 124 ] && fail "hung: $*"
	return $_rc
}

$TMUX -fNUL new -d </dev/null || fail "new"

printf 'set -g @one 1\r\nset -g @two 2\n' >$OUT/good.conf
run source-file $OUT/good.conf || fail "source-file"
[ "$($TMUX show -gv @one)" = 1 ] || fail "@one not set"
[ "$($TMUX show -gv @two)" = 2 ] || fail "@two not set"

printf 'set -g @three 3\nno-such-command\n' >$OUT/bad.conf
run source-file $OUT/bad.conf >/dev/null 2>&1 && fail "bad file exited 0"
run source-file $OUT/missing.conf 2>/dev/null && fail "missing file exited 0"

printf 'a\r\nb\000c' >$OUT/in.bin
run load-buffer -b in $OUT/in.bin || fail "load-buffer"
run save-buffer -b in $OUT/out.bin || fail "save-buffer"
cmp -s $OUT/in.bin $OUT/out.bin || fail "save-buffer changed the bytes"

$TMUX kill-server 2>/dev/null

echo "ALL FILE TESTS PASSED"
exit 0
