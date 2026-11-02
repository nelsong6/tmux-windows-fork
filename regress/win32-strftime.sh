#!/bin/sh

# A % sequence the C runtime's strftime doesn't know is printed as written,
# as glibc does, and the server keeps running; the format it was in is
# recorded once in the server's messages (TMUX-37). Before the fix any of
# these ended the server.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest-strftime"
. "$(dirname "$0")/win32-wait.sh"
$TMUX kill-server 2>/dev/null
wait_gone

trap "$TMUX kill-server 2>/dev/null" 0 1 15

fail() {
	echo "FAIL: $1"
	exit 1
}

# expect FORMAT PATTERN: display -p FORMAT prints a line matching PATTERN
# and the server is still running.
expect() {
	out=$($TMUX display-message -p "$1" 2>&1 | tr -d '\r')
	$TMUX has-session 2>/dev/null || fail "server ended on '$1'"
	echo "$out" | grep -qx -- "$2" ||
	    fail "'$1' printed '$out', expected '$2'"
}

$TMUX -fNUL new -d </dev/null || fail "new-session"

# Time codes the C runtime knows still expand.
expect 'ok %H:%M' 'ok [0-9][0-9]:[0-9][0-9]'
expect '%#d %Ey %OH 100%%' '[0-9]* [0-9][0-9] [0-9][0-9] 100%'

# Unknown sequences are left as they were.
expect 'sale 50%Q off' 'sale 50%Q off'
expect 'pad %-H trailing %' 'pad %-H trailing %'
expect 'popup -w 90% -h 80%' 'popup -w 90% -h 80%'

# An option reached through E: (how @at-menu ended the server).
$TMUX set -g @x 'a 90% b' || fail "set @x"
expect '#{E:@x}' 'a 90% b'

# The t/ modifier's own time format (%% reaches it as %).
expect '#{t/f/%%Q:start_time}' '%Q'

# Each format with an unknown sequence is recorded once.
expect 'sale 50%Q off' 'sale 50%Q off'
n=$($TMUX show-messages | tr -d '\r' |
    grep -c 'left as text: sale 50%Q off$')
[ "$n" = 1 ] || fail "'sale 50%Q off' recorded $n times, expected 1"
$TMUX show-messages | tr -d '\r' | grep -q 'left as text: a 90% b$' ||
    fail "@x's value not recorded"

$TMUX kill-server 2>/dev/null

echo "ALL STRFTIME TESTS PASSED"
exit 0
