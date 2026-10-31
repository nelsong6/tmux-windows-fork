#!/bin/sh

# switch-client -H and -R (a chord) on macOS: the table lasts while the key's
# modifiers are physically held, and -R runs when they are released. Nothing
# holds Ctrl here, so a chord's release comes on the first check after its
# first key; a second key sent in the same command list arrives before it.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
TMUX_OUTER="$TEST_TMUX -Ltest-outer"
. "$(dirname "$0")/win32-wait.sh"
$TMUX kill-server 2>/dev/null
$TMUX_OUTER kill-server 2>/dev/null
wait_gone

trap "$TMUX kill-server 2>/dev/null; $TMUX_OUTER kill-server 2>/dev/null" 0 1 15

$TMUX -f/dev/null new -d -x100 -y30 -n first </dev/null || exit 1
$TMUX bind -n C-t switch-client -T ctrl-t -H -R 'new-window -n plain' ||
    exit 1
$TMUX bind -T ctrl-t C-a new-window -n leaf || exit 1

$TMUX_OUTER -f/dev/null new -d -x100 -y30 "$TMUX attach" </dev/null || exit 1
attached() {
	[ -n "$($TMUX list-clients 2>/dev/null)" ]
}
wait_until attached || { echo "client never attached"; exit 1; }
CLIENT=$($TMUX list-clients -F '#{client_name}')

has_window() {
	$TMUX list-windows -F '#{window_name}' | grep -qx "$1"
}
window_count() {
	$TMUX list-windows | wc -l | tr -d ' '
}
in_root() {
	[ "$($TMUX display -c "$CLIENT" -p '#{client_key_table}')" = root ]
}

# -H needs a key with a modifier.
$TMUX switch-client -c "$CLIENT" -T ctrl-t -H 2>/dev/null &&
    { echo "-H without a key was accepted"; exit 1; }

# Released with nothing after it: -R runs.
$TMUX send-keys -K -c "$CLIENT" C-t
wait_until has_window plain || { echo "release did not run -R"; exit 1; }
wait_until in_root || { echo "the table outlived the release"; exit 1; }
$TMUX kill-window -t plain

# A leaf before the release runs, and -R does not.
$TMUX send-keys -K -c "$CLIENT" C-t \; send-keys -K -c "$CLIENT" C-a
wait_until has_window leaf || { echo "the leaf did not run"; exit 1; }
sleep 0.2
has_window plain && { echo "-R ran after a leaf"; exit 1; }
$TMUX kill-window -t leaf

# Any other key ends the chord with nothing run.
$TMUX send-keys -K -c "$CLIENT" C-t \; send-keys -K -c "$CLIENT" x
sleep 0.2
[ "$(window_count)" -eq 1 ] || { echo "a key outside the chord ran something"; exit 1; }
wait_until in_root || { echo "the table outlived an unbound key"; exit 1; }

$TMUX kill-server 2>/dev/null

echo "ALL HOLD TESTS PASSED"
exit 0
