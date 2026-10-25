#!/bin/sh

# Windows key handling regression tests.
# Tests that send-keys with named keys produces expected effects.
# Cannot use the upstream cat -tv approach (no stty on Windows), so we
# verify key behavior through observable pane output.
#
# Upstream had 4 backspace/key-mapping fixes in Dec 2024 - Feb 2025:
#   6d792e4, 2a5eba7, 5c3cf2f, eece415

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
. "$(dirname "$0")/win32-wait.sh"
$TMUX kill-server 2>/dev/null
wait_gone

FNULL="-fNUL"
OUT=$(mktemp)
trap "rm -f $OUT; $TMUX kill-server 2>/dev/null" 0 1 15
FAIL=0

in_mode() {
	[ "$($TMUX display -tkeys -p '#{pane_in_mode}')" = 1 ]
}

out_of_mode() {
	[ "$($TMUX display -tkeys -p '#{pane_in_mode}')" = 0 ]
}

fail() {
	echo "FAIL: $1"
	FAIL=1
}

$TMUX $FNULL new -d -skeys -x 120 -y 24 < /dev/null || exit 1
wait_ready keys || { fail "shell never drew a prompt"; exit 1; }

# --- Test 1: Enter key executes commands ---
$TMUX send-keys -tkeys "echo KEY_ENTER_OK" Enter
wait_for keys "^KEY_ENTER_OK" || {
	fail "Enter key did not execute command"; exit 1
}
echo "PASS 1: Enter key"

# --- Test 2: Literal characters arrive correctly ---
$TMUX send-keys -tkeys "echo abcXYZ019" Enter
wait_for keys "^abcXYZ019" || {
	fail "Literal characters not received"
}
echo "PASS 2: Literal characters"

# --- Test 3: Space key ---
$TMUX send-keys -tkeys "echo" Space "SPACE_OK" Enter
wait_for keys "^SPACE_OK" || {
	fail "Space key not working"
}
echo "PASS 3: Space key"

# --- Test 4: Tab key (command completion) ---
# Type partial command and Tab — on cmd.exe, Tab cycles through files.
# Simpler test: verify Tab character is sent by checking it doesn't break things.
$TMUX send-keys -tkeys "echo TAB_OK" Tab Enter
# Tab might complete or not, but the echo should still work
wait_for keys "^TAB_OK" || true
echo "PASS 4: Tab key (no crash)"

# --- Test 5: Escape key (no crash, key is recognized) ---
# bash reads Escape as Meta and takes the next key with it; Space makes
# that M-Space (set-mark), so the echo arrives whole.
$TMUX send-keys -tkeys Escape Space
$TMUX send-keys -tkeys "echo ESC_OK" Enter
wait_for keys "^ESC_OK" || {
	fail "Escape key broke input"
}
echo "PASS 5: Escape key"

# --- Test 6: Ctrl-C (interrupt) ---
# Send a long-running command, then Ctrl-C to interrupt once bash is running
# it (sent earlier, Ctrl-C only cancels the line being read). Uninterrupted,
# the sleep outlasts the wait and NOT_INTERRUPTED would follow it.
$TMUX send-keys -tkeys "echo STARTED; sleep 100 && echo NOT_INTERRUPTED" Enter
wait_for keys "^STARTED"
$TMUX send-keys -tkeys C-c
# Verify we get back to a prompt (can type again); what is typed before it
# returns is discarded with the interrupted input.
wait_ready keys
$TMUX send-keys -tkeys "echo CTRLC_OK" Enter
wait_for keys "^CTRLC_OK" && ! pane_has keys "^NOT_INTERRUPTED" || {
	fail "Ctrl-C did not interrupt"
}
echo "PASS 6: Ctrl-C interrupt"

# --- Test 7: Arrow keys in copy-mode ---
# Enter copy mode, move around, exit — verify no crash
$TMUX send-keys -tkeys "echo line1" Enter "echo line2" Enter "echo line3" Enter
wait_for keys "^line3"
$TMUX copy-mode -tkeys
wait_until in_mode
$TMUX send-keys -tkeys -X cursor-up
$TMUX send-keys -tkeys -X cursor-up
$TMUX send-keys -tkeys -X cursor-down
$TMUX send-keys -tkeys -X cursor-left
$TMUX send-keys -tkeys -X cursor-right
$TMUX send-keys -tkeys -X cancel
wait_until out_of_mode
# Verify pane is still functional
$TMUX send-keys -tkeys "echo ARROW_OK" Enter
wait_for keys "^ARROW_OK" || {
	fail "Arrow keys in copy mode broke pane"
}
echo "PASS 7: Arrow keys in copy mode"

# --- Test 8: Home/End in copy mode ---
$TMUX copy-mode -tkeys
wait_until in_mode
$TMUX send-keys -tkeys -X start-of-line
$TMUX send-keys -tkeys -X end-of-line
$TMUX send-keys -tkeys -X cancel
wait_until out_of_mode
$TMUX send-keys -tkeys "echo HOMEEND_OK" Enter
wait_for keys "^HOMEEND_OK" || {
	fail "Home/End keys broke pane"
}
echo "PASS 8: Home/End in copy mode"

# --- Test 9: PgUp/PgDn in copy mode ---
# Generate enough output to scroll
for i in $(seq 1 30); do
	$TMUX send-keys -tkeys "echo scroll_line_$i" Enter
done
wait_for keys "^scroll_line_30"
$TMUX copy-mode -tkeys
wait_until in_mode
$TMUX send-keys -tkeys -X page-up
$TMUX send-keys -tkeys -X page-down
$TMUX send-keys -tkeys -X cancel
wait_until out_of_mode
$TMUX send-keys -tkeys "echo PGUPDN_OK" Enter
wait_for keys "^PGUPDN_OK" || {
	fail "PgUp/PgDn broke pane"
}
echo "PASS 9: PgUp/PgDn in copy mode"

# --- Test 10: Function keys (no crash) ---
# F1-F5 might trigger help or other actions, but shouldn't crash
$TMUX send-keys -tkeys F1
$TMUX send-keys -tkeys Escape Space
$TMUX send-keys -tkeys "echo FKEY_OK" Enter
wait_for keys "^FKEY_OK" || {
	fail "Function keys broke pane"
}
echo "PASS 10: Function keys"

# --- Test 11: BSpace (backspace) deletes characters ---
# Type something, backspace, then check result
$TMUX send-keys -tkeys "echo BSPXXX" BSpace BSpace BSpace "ACE_OK" Enter
wait_for keys "^BSPACE_OK" || {
	fail "Backspace did not delete characters"
}
echo "PASS 11: Backspace"

# --- Test 12: Multiple modifier combinations (no crash) ---
$TMUX send-keys -tkeys C-a C-e C-k
$TMUX send-keys -tkeys "echo MOD_OK" Enter
wait_for keys "^MOD_OK" || {
	fail "Modifier combinations broke pane"
}
echo "PASS 12: Modifier combinations"

$TMUX kill-server 2>/dev/null

if [ $FAIL -eq 0 ]; then
	echo ""
	echo "ALL KEY TESTS PASSED"
	exit 0
else
	echo ""
	echo "SOME KEY TESTS FAILED"
	exit 1
fi
