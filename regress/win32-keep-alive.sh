#!/bin/sh

# keep-alive-window: a session that loses its last window by any route but
# kill-session opens the named window in its place, and a replacement that dies
# straight away is not replaced again.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
. "$(dirname "$0")/win32-wait.sh"
$TMUX kill-server 2>/dev/null
wait_gone

FNULL="-fNUL"
trap "$TMUX kill-server 2>/dev/null" 0 1 15

fail() {
	echo "FAIL: $1"
	$TMUX ls 2>&1
	$TMUX lsw -a -F '#{session_name}:#{window_index} #{window_name}' 2>&1
	exit 1
}

# windows_are SESSION NAMES: SESSION's window names, space-separated.
windows_are() {
	[ "$($TMUX lsw -t"=$1" -F '#{window_name}' 2>/dev/null | tr -d '\r' |
	    tr '\n' ' ' | sed 's/ $//')" = "$2" ]
}

session_gone() {
	! $TMUX has -t"=$1" 2>/dev/null
}

# A second session keeps the server up when the tested one ends.
$TMUX $FNULL new -d -s other -n o </dev/null || fail "start server"
$TMUX new -d -s p -n first || fail "new session"
$TMUX set -t p keep-alive-window autolaunched || fail "set option"

# kill-window on the last window.
$TMUX kill-window -t p:first || fail "kill-window"
windows_are p autolaunched || fail "kill-window: p has no autolaunched window"
$TMUX display -p -t p '#{automatic-rename}' | tr -d '\r' | grep -q '^0$' ||
    fail "kill-window: autolaunched window renames itself"

# The shell in the last window exits. The replacement's shell has to be older
# than the one-second guard for its exit to be replaced.
sleep 1.1
wait_ready p
$TMUX send-keys -t p 'exit' Enter
wait_until windows_are p autolaunched || fail "shell exit: not replaced"
$TMUX display -p -t p '#{window_id}' >/dev/null || fail "shell exit: p gone"

# The last window moved to another session, straight after it opened: only a
# program ending is held to the one-second guard.
$TMUX rename-window -t p moved
$TMUX move-window -s p:moved -t other: || fail "move-window"
windows_are p autolaunched || fail "move-window: p not refilled"
windows_are other "o moved" || fail "move-window: other lacks the moved window"

# A replacement whose command exits at once is not replaced again.
$TMUX set -t p default-command 'cmd.exe /c exit'
$TMUX kill-window -t p || fail "kill-window before quick exit"
wait_until session_gone p || fail "quick exit: p respawned"
windows_are other "o moved" || fail "quick exit: other changed"

# kill-session still ends a kept-alive session.
$TMUX new -d -s p -n first || fail "new session again"
$TMUX set -t p keep-alive-window autolaunched
$TMUX kill-session -t p || fail "kill-session"
session_gone p || fail "kill-session: p still there"

# Without the option, the session ends with its last window.
$TMUX new -d -s q -n only || fail "new q"
$TMUX kill-window -t q:only
session_gone q || fail "no option: q still there"

$TMUX kill-server 2>/dev/null

echo "ALL KEEP-ALIVE TESTS PASSED"
exit 0
