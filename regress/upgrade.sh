#!/bin/sh

# upgrade-server on Linux: a running server hands everything to another
# process of a build and nothing in it restarts. The server under test is
# attached to by a real client, which runs in a pane of a second server.
# Upgrades go to the same build: what matters is the handover.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Lupgtest"
. "$(dirname "$0")/win32-wait.sh"
OUTER="$TEST_TMUX -Lupgouter"
$TMUX kill-server 2>/dev/null
$OUTER kill-server 2>/dev/null
wait_gone

CFG=$(mktemp)
printf "set -g default-shell /bin/sh\n" >$CFG
OUT=$(mktemp)
trap "rm -f $CFG $OUT; $TMUX kill-server 2>/dev/null; $OUTER kill-server 2>/dev/null" 0 1 15
FAIL=0
EXE=$(readlink -f "$TEST_TMUX")
export PS1='$ '

fail() {
	echo "FAIL: $1"
	FAIL=1
}

server_pid() {
	$TMUX display -p '#{pid}' 2>/dev/null
}

pane_pids() {
	$TMUX list-panes -a -F '#{pane_id}=#{pane_pid}'
}

upgrade_status() {
	$TMUX display -p '#{server_upgrade}' 2>/dev/null
}

pid_changed_from() {
	_p=$(server_pid)
	[ -n "$_p" ] && [ "$_p" != "$1" ]
}

outer_has() {
	$OUTER capture-pane -tout -p 2>/dev/null | grep -q -- "$1"
}

client_count_is() {
	[ "$($TMUX list-clients 2>/dev/null | wc -l)" -eq "$1" ]
}

status_starts() {
	upgrade_status | grep -q "^$1"
}

$TMUX -f "$CFG" new -d -sup -x 100 -y 30 < /dev/null || exit 1
wait_ready up || { fail "shell never drew a prompt"; exit 1; }
$TMUX splitw -tup < /dev/null
$TMUX neww -tup -n second < /dev/null
$TMUX selectw -tup:0
wait_panes up:0 2
wait_ready up:0.1
$TMUX send-keys -tup:0.0 "echo BEFORE_MARK" Enter
wait_for up:0.0 "^BEFORE_MARK"
$TMUX set -g @upgrade-test kept
$TMUX bind -T root F12 display -p f12-bound
$TMUX set-buffer -b upbuf "buffer kept"
# As a server upgraded by an affected build carries it in its state.
$TMUX setenv -g TMUX_UPGRADE 1,2
# A variable removed from the global environment, and one added to it.
$TMUX setenv -g -u LOGNAME
$TMUX setenv -g UPG_ADDED kept

# A real client, attached from a pane of the outer server.
$OUTER -f "$CFG" new -d -sout -x 100 -y 32 < /dev/null || exit 1
$OUTER set -g status off
wait_until outer_has '\$' || { fail "outer shell never drew a prompt"; exit 1; }
$OUTER send-keys -tout "unset TMUX; $EXE -Lupgtest attach -tup" Enter
wait_until client_count_is 1 || { fail "client never attached"; exit 1; }
wait_until outer_has BEFORE_MARK || fail "client never drew the pane"

COLOURS0=$($TMUX display -tup:0.0 -p '#{pane_fg} #{pane_bg}')

PID0=$(server_pid)
pane_pids >$OUT
PANES0=$(cat $OUT)

# --- Test 1: upgrade keeps processes, screens, state and the client ---
$TMUX upgrade-server "$EXE" || fail "upgrade-server refused"
wait_until pid_changed_from "$PID0" || fail "server did not change: $(upgrade_status)"
PID1=$(server_pid)
[ "$(pane_pids)" = "$PANES0" ] || fail "pane processes changed: $(pane_pids)"
pane_has up:0.0 "^BEFORE_MARK" || fail "pane screen lost"
[ "$($TMUX show -gv @upgrade-test)" = kept ] || fail "option lost"
[ "$($TMUX show-buffer -b upbuf)" = "buffer kept" ] || fail "buffer lost"
$TMUX list-keys -T root F12 | grep -q f12-bound || fail "key binding lost"
[ "$($TMUX list-windows -tup | wc -l)" -eq 2 ] || fail "window lost"
[ "$($TMUX display -tup:1 -p '#{window_name}')" = second ] ||
	fail "window name lost"
client_count_is 1 || fail "client lost"
# A pane's default colours are worked out again, not left as zeroes (black
# on black), once the new server has settled.
sleep 2.5
[ "$($TMUX display -tup:0.0 -p '#{pane_fg} #{pane_bg}')" = "$COLOURS0" ] ||
	fail "pane colours changed: $($TMUX display -tup:0.0 -p '#{pane_fg} #{pane_bg}'), were $COLOURS0"
echo "PASS 1: upgrade keeps panes, screens, options, buffers, keys, client and pane colours"

# --- Test 1b: the environment is the one before, less TMUX_UPGRADE ---
$TMUX show-environment -g TMUX_UPGRADE >/dev/null 2>&1 &&
	fail "TMUX_UPGRADE in the global environment"
$TMUX show-environment -g LOGNAME >/dev/null 2>&1 &&
	fail "a removed variable came back"
[ "$($TMUX show-environment -g UPG_ADDED)" = UPG_ADDED=kept ] ||
	fail "an added variable lost"
$TMUX neww -tup -n envcheck < /dev/null
wait_ready up:envcheck
$TMUX send-keys -tup:envcheck \
    'echo UPG_${TMUX_UPGRADE:-UNSET} USER_${LOGNAME:-UNSET}' Enter
wait_for up:envcheck "^UPG_UNSET USER_UNSET" ||
	fail "a new pane inherits TMUX_UPGRADE or a removed variable"
$TMUX kill-window -tup:envcheck
echo "PASS 1b: the global environment and a new pane's are as before the upgrade"

# --- Test 2: the panes and the client work after the upgrade ---
$TMUX send-keys -tup:0.0 "echo AFTER_MARK" Enter
wait_for up:0.0 "^AFTER_MARK" || fail "pane input or output broken"
wait_until outer_has AFTER_MARK || fail "client not drawn after upgrade"
# The client types into the active pane, the split.
$OUTER send-keys -tout "echo TYPED_MARK" Enter
wait_for up:0.1 "^TYPED_MARK" || fail "client keys not reaching pane"
echo "PASS 2: panes and client work after upgrade"

# --- Test 3: it waits while copy mode is open, and goes once it closes ---
$TMUX copy-mode -tup:0.0
$TMUX upgrade-server "$EXE"
wait_until status_starts "waiting: pane" || fail "did not wait: $(upgrade_status)"
[ "$(server_pid)" = "$PID1" ] || fail "upgraded during copy mode"
$TMUX send-keys -tup:0.0 -X cancel
wait_until pid_changed_from "$PID1" || fail "did not upgrade after copy mode"
PID2=$(server_pid)
[ "$(pane_pids)" = "$PANES0" ] || fail "pane processes changed on second upgrade"
echo "PASS 3: waits for copy mode, upgrades again"

# --- Test 4: a build that can't take over leaves the server as it was ---
$TMUX upgrade-server /bin/false
wait_until status_starts "failed" || fail "no failure reported: $(upgrade_status)"
[ "$(server_pid)" = "$PID2" ] || fail "server changed after a failed upgrade"
$TMUX send-keys -tup:0.0 "echo ROLLBACK_MARK" Enter
wait_for up:0.0 "^ROLLBACK_MARK" || fail "pane broken after a failed upgrade"
wait_until outer_has ROLLBACK_MARK || fail "client broken after a failed upgrade"
echo "PASS 4: failed upgrade carries on"

# --- Test 5: a missing build is refused at once ---
$TMUX upgrade-server /no/such/tmux 2>$OUT && fail "missing build accepted"
grep -q "no such file" $OUT || fail "missing build: $(cat $OUT)"
echo "PASS 5: missing build refused"

$TMUX kill-server 2>/dev/null
$OUTER kill-server 2>/dev/null

if [ $FAIL -eq 0 ]; then
	echo ""
	echo "ALL UPGRADE TESTS PASSED"
	exit 0
else
	echo ""
	echo "SOME UPGRADE TESTS FAILED"
	exit 1
fi
