#!/bin/sh

# new-window -W: the window opens in the background and the named client is
# switched to it only once its pane has drawn something. A pane whose console
# has only done its own setup stays unswitched, one whose program exits first
# fails the command, and killing the waiting command (or the window) gives up
# the wait with the server still running.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Lfzt64test-nwdraw"
. "$(dirname "$0")/win32-wait.sh"
$TMUX kill-server 2>/dev/null
wait_gone

# The panes' commands are cmd.exe's, whatever SHELL the tests run under.
# PATH is only MSYS's here, so ping goes by its full path; it waits a second
# per count.
PING='%SystemRoot%\System32\ping.exe -n'
CFG=$(mktemp)
printf "set -g default-shell '%s'\n" "$COMSPEC" >$CFG
OUT=$(mktemp)
CTL=$(mktemp)
FAIL=0
SLEEPER=$(mktemp)
cleanup() {
	$TMUX kill-server 2>/dev/null
	[ -s $SLEEPER ] && kill "$(cat $SLEEPER)" 2>/dev/null
	rm -f $CFG $OUT $CTL $SLEEPER
}
trap cleanup 0 1 15

TFAIL=0
fail() {
	echo "FAIL: $1"
	FAIL=1
	TFAIL=1
}

# pass MESSAGE: the test that has just run passed, if nothing in it failed.
pass() {
	[ $TFAIL -eq 0 ] && echo "PASS $1"
	TFAIL=0
}

# The window the client shows, by name.
client_window() {
	$TMUX list-clients -F '#{window_name}' 2>/dev/null | tr -d '\r'
}

client_on() {
	[ "$(client_window)" = "$1" ]
}

window_exists() {
	$TMUX lsw -t w -F '#{window_name}' 2>/dev/null | tr -d '\r' |
	    grep -qx -- "$1"
}

# Is the background process $1 still running?
running() {
	kill -0 "$1" 2>/dev/null
}

$TMUX -f$CFG new -d -s w -n home -x 80 -y 24 </dev/null || exit 1
$TMUX set -g remain-on-exit off
# A control client stands in for the terminal; it stays attached while its
# input is open.
(sleep 300 >/dev/null 2>&1 & echo $! >$SLEEPER; wait) |
    $TMUX -C attach -t w >$CTL 2>&1 &
wait_until [ -n "$(client_window)" ] || { fail "no client"; exit 1; }
C=$($TMUX list-clients -F '#{client_name}' | tr -d '\r')
client_on home || fail "client starts on $(client_window)"

# --- Test 1: a pane that draws after a second. The client stays where it is
# until the pane shows its text, and the command waits until then. ---
$TMUX new-window -a -t w:home -n late -P -F '#{window_name}' -W "$C" \
    "$PING 3 127.0.0.1 >nul & echo drawn & $PING 300 127.0.0.1 >nul" \
    >$OUT 2>&1 &
W=$!
wait_until window_exists late || fail "1: no window"
client_on home || fail "1: switched before drawing: $(client_window)"
running $W || fail "1: returned before drawing: $(cat $OUT)"
pane_has w:late '^drawn' && fail "1: drew too early"
wait_until pane_has w:late '^drawn' || fail "1: pane never drew"
wait $W
RC=$?
[ $RC -eq 0 ] || fail "1: exit $RC: $(cat $OUT)"
tr -d '\r' <$OUT | grep -qx late || fail "1: -P printed $(cat $OUT)"
client_on late || fail "1: not switched after drawing: $(client_window)"
pass "1: switched once the pane drew"
$TMUX select-window -t w:home

# --- Test 2: a program that exits without drawing fails the command and
# switches nothing. ---
$TMUX new-window -a -t w:home -n quick -W "$C" 'exit 3' >$OUT 2>&1
RC=$?
[ $RC -ne 0 ] || fail "2: exit 0 for a pane that never drew"
tr -d '\r' <$OUT | grep -q 'exited before drawing' || \
    fail "2: message: $(cat $OUT)"
client_on home || fail "2: switched to $(client_window)"
pass "2: exit before drawing"

# --- Test 3: a console with only its own setup (a program that writes
# nothing) leaves the client unswitched; killing the waiting command gives the
# wait up and the server carries on. ---
$TMUX new-window -a -t w:home -n silent -W "$C" \
    "$PING 300 127.0.0.1 >nul" >$OUT 2>&1 &
W=$!
wait_until window_exists silent || fail "3: no window"
sleep 2
client_on home || fail "3: switched to a blank pane: $(client_window)"
running $W || fail "3: returned for a blank pane: $(cat $OUT)"
"$(cygpath "$SYSTEMROOT")/System32/taskkill.exe" //F \
    //PID "$(cat /proc/$W/winpid)" >/dev/null 2>&1 ||
    fail "3: could not kill the waiting command"
wait $W 2>/dev/null
$TMUX display -p '#{pid}' >/dev/null 2>&1 || fail "3: server gone"
window_exists silent || fail "3: window gone"
client_on home || fail "3: switched after giving up: $(client_window)"
# The window is ordinary now: its pane drawing later switches nothing.
$TMUX send-keys -t w:silent C-c 2>/dev/null
$TMUX respawn-pane -k -t w:silent "echo later & $PING 300 127.0.0.1 >nul"
wait_until pane_has w:silent '^later' || fail "3: respawned pane never drew"
client_on home || fail "3: an abandoned wait switched: $(client_window)"
pass "3: a blank pane is not shown; the wait can be abandoned"

# --- Test 4: the window is killed while its command waits. ---
$TMUX new-window -a -t w:home -n killed -W "$C" \
    "$PING 300 127.0.0.1 >nul" >$OUT 2>&1 &
W=$!
wait_until window_exists killed || fail "4: no window"
$TMUX kill-window -t w:killed
wait $W
RC=$?
[ $RC -ne 0 ] || fail "4: exit 0 for a killed window"
tr -d '\r' <$OUT | grep -q 'closed before drawing' || \
    fail "4: message: $(cat $OUT)"
client_on home || fail "4: switched to $(client_window)"
pass "4: window killed while waiting"

# --- Test 5: a client that doesn't exist is refused before a window opens. ---
$TMUX new-window -a -t w:home -n nobody -W no-such-client 'echo x' \
    >$OUT 2>&1 && fail "5: unknown client accepted"
window_exists nobody && fail "5: window opened for an unknown client"
pass "5: unknown client"

$TMUX kill-server 2>/dev/null

if [ $FAIL -eq 0 ]; then
	echo ""
	echo "ALL NEW-WINDOW -W TESTS PASSED"
	exit 0
fi
echo ""
echo "SOME NEW-WINDOW -W TESTS FAILED"
exit 1
