#!/bin/sh

# Control mode test for Windows.
# Tests 1-6 are the pane operations of upstream control-client-sanity.sh,
# run as normal commands. Tests 7-9 drive a -C client: replies, option
# subscriptions and exit at end of input; test 10 a held key table (-H, -R);
# test 11 the global key table.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Lctrltest"
. "$(dirname "$0")/win32-wait.sh"
$TMUX kill-server 2>/dev/null
wait_gone

FNULL="-fNUL"
OUT=$(mktemp)
trap "rm -f $OUT; $TMUX kill-server 2>/dev/null" 0 1 15
FAIL=0

fail() {
	echo "FAIL: $1"
	FAIL=1
}

$TMUX $FNULL new -d -sctrl -x 200 -y 200 < /dev/null || exit 1

# --- Test 1: Complex pane operations (equivalent to control-client-sanity) ---
$TMUX splitw -tctrl || { fail "splitw"; exit 1; }
$TMUX selectp -tctrl -t0 || { fail "selectp 0"; exit 1; }
$TMUX splitw -tctrl || { fail "splitw 2"; exit 1; }
$TMUX neww -tctrl || { fail "neww"; exit 1; }
$TMUX splitw -tctrl || { fail "splitw 3"; exit 1; }
echo "PASS 1: pane operations"

# --- Test 2: Kill pane ---
# Get pane IDs
$TMUX lsp -tctrl -F '#{pane_id}' | tr -d '\r' >$OUT
BEFORE=$(wc -l < $OUT)
KILL_PANE=$(tail -1 $OUT)
$TMUX killp -tctrl -t"$KILL_PANE" || { fail "killp"; exit 1; }
AFTER=$($TMUX lsp -tctrl | wc -l)
[ "$AFTER" -eq $((BEFORE - 1)) ] || fail "killp: $BEFORE -> $AFTER (expected $((BEFORE - 1)))"
echo "PASS 2: kill pane ($BEFORE -> $AFTER)"

# --- Test 3: Swap panes ---
$TMUX lsp -tctrl -F '#{pane_id}' | tr -d '\r' >$OUT
P1=$(sed -n '1p' $OUT)
$TMUX splitw -tctrl || exit 1
$TMUX lsp -tctrl -F '#{pane_id}' | tr -d '\r' >$OUT
P2=$(tail -1 $OUT)
$TMUX swapp -s "$P1" -t "$P2" || { fail "swapp"; exit 1; }
echo "PASS 3: swap panes"

# --- Test 4: Select layout tiled ---
$TMUX neww -tctrl || exit 1
$TMUX splitw -tctrl || exit 1
$TMUX splitw -tctrl || exit 1
$TMUX splitw -tctrl || exit 1
$TMUX selectl -tctrl tiled || { fail "selectl tiled"; exit 1; }
COUNT=$($TMUX lsp -tctrl | wc -l)
[ "$COUNT" -eq 4 ] || fail "tiled: expected 4 panes, got $COUNT"
echo "PASS 4: select-layout tiled ($COUNT panes)"

# --- Test 5: Kill window ---
WCOUNT_BEFORE=$($TMUX lsw -tctrl | wc -l)
$TMUX killw -tctrl || { fail "killw"; exit 1; }
WCOUNT_AFTER=$($TMUX lsw -tctrl | wc -l)
[ "$WCOUNT_AFTER" -eq $((WCOUNT_BEFORE - 1)) ] || \
	fail "killw: $WCOUNT_BEFORE -> $WCOUNT_AFTER"
echo "PASS 5: kill window ($WCOUNT_BEFORE -> $WCOUNT_AFTER)"

# --- Test 6: Verify server still alive after all operations ---
$TMUX has -tctrl || { fail "server died"; exit 1; }

# Verify layout output is sane
$TMUX lsp -tctrl -aF '#{pane_id} #{window_layout}' | tr -d '\r' >$OUT
COUNT=$(wc -l < $OUT)
[ "$COUNT" -ge 1 ] || { fail "no panes in final layout"; exit 1; }
grep -q '%' $OUT || { fail "no pane IDs in layout output"; exit 1; }
echo "PASS 6: server alive, layout sane ($COUNT pane lines)"

# --- Test 7: control client gets command replies ---
CTL=$(mktemp)
trap "rm -f $OUT $CTL; $TMUX kill-server 2>/dev/null" 0 1 15
ctl_has() {
	tr -d '\r' <$CTL | grep -q -- "$1"
}
$TMUX set -g @tb start
(
	echo 'display -p ctl-ok'
	echo "refresh-client -B 'tb::#{@tb}'"
	wait_until ctl_has ': start$'
	$TMUX set -g @tb clicked
	wait_until ctl_has ': clicked$'
) | $TMUX -C attach -tctrl >$CTL 2>&1 &
wait_until ctl_has '^ctl-ok$' || fail "control reply: $(cat $CTL)"
echo "PASS 7: control client command reply"

# --- Test 8: control client is notified of a subscribed option ---
wait_until ctl_has '^%subscription-changed tb .*: clicked$' || \
	fail "subscription: $(cat $CTL)"
echo "PASS 8: control client subscription"

# --- Test 9: control client exits at end of input ---
wait_until ctl_has '^%exit' || fail "control exit: $(cat $CTL)"
$TMUX has -tctrl || fail "server died with control client"
echo "PASS 9: control client exits, server alive"

# --- Test 10: switch-client -H holds a key table only while the key's
# modifiers are down, then runs -R. Nothing holds Ctrl here, so a held table
# ends at once and runs -R; -H with no key behind it, and -R without -H, are
# refused. ---
: >$CTL
$TMUX bind -Theld x display held-x
$TMUX bind -n C-t switch-client -Theld -H -R 'set -g @released yes'
(
	echo 'switch-client -Theld -H'
	echo 'switch-client -Theld -R new-window'
	echo 'send-keys -K C-t'
	sleep 1
	echo "display -p 'table=#{client_key_table} released=#{@released}'"
	wait_until ctl_has '^table='
) | $TMUX -C attach -tctrl >$CTL 2>&1 &
wait_until ctl_has '^table=' || fail "hold: no reply: $(cat $CTL)"
ctl_has '^-H needs a key with a modifier$' || \
	fail "hold: -H without a key was not refused: $(cat $CTL)"
ctl_has '^-R needs -H$' || \
	fail "hold: -R without -H was not refused: $(cat $CTL)"
ctl_has '^table=root released=yes$' || \
	fail "hold: table stayed, or -R didn't run, with Ctrl up: $(cat $CTL)"
wait
echo "PASS 10: switch-client -H -R"

# --- Test 11: the global table takes W- keys and runs them for the attached
# client when they are pressed anywhere on the desktop; a server with no
# client ignores them. The press is simulated only in CI, which has a desktop
# of its own; on a developer's PC it would land on theirs. ---
: >$CTL
IDLE="$TEST_TMUX -Lctrltest-idle"
$IDLE $FNULL new -d -sidle < /dev/null || fail "global: idle server"
$IDLE bind -T global 'W-`' set -g @global pressed
$TMUX bind -T global 'W-`' set -g @global pressed
$TMUX list-keys -T global | tr -d '\r' | grep -q 'global *W-` ' || \
	fail "global: W-\` not listed: $($TMUX list-keys -T global)"
if [ -n "$CI" ]; then
	(
		echo "display -p 'attached'"
		wait_until [ -n "$($TMUX show -gv @global 2>/dev/null)" ]
		echo "display -p 'global=#{@global}'"
		wait_until ctl_has '^global='
	) | $TMUX -C attach -tctrl >$CTL 2>&1 &
	wait_until ctl_has '^attached$' || fail "global: no client: $(cat $CTL)"
	PS="$(cygpath "$SYSTEMROOT")/System32/WindowsPowerShell/v1.0/powershell.exe"
	PRESS=$("$PS" -NoProfile -ExecutionPolicy Bypass \
	    -File "$(dirname "$0")/win32-press.ps1" 2>&1 | tr -d '\r')
	echo "press: $PRESS"
	wait_until ctl_has '^global=' || fail "global: no reply: $(cat $CTL)"
	ctl_has '^global=pressed$' || \
		fail "global: Win+\` didn't run the binding: $(cat $CTL)"
	[ -z "$($IDLE show -gv @global 2>/dev/null)" ] || \
		fail "global: a server with no client ran the binding"
	wait
	echo "PASS 11: global key table"
else
	echo "PASS 11: global key table (listed; press simulated only in CI)"
fi
$IDLE kill-server 2>/dev/null

$TMUX kill-server 2>/dev/null

if [ $FAIL -eq 0 ]; then
	echo ""
	echo "ALL CONTROL CLIENT TESTS PASSED"
	exit 0
else
	echo ""
	echo "SOME CONTROL CLIENT TESTS FAILED"
	exit 1
fi
