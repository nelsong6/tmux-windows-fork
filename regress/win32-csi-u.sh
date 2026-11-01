#!/bin/sh

# A key the terminal sends in the kitty keyboard protocol's short form,
# ESC [ key u with the modifier left out, is read as that key (TMUX-30:
# Windows Terminal sends Esc as ESC [ 27 u once tmux asks for the protocol,
# and tmux read it as M-[ and then typed 27u into the pane). The client runs
# in a pane of a second server, which stands in for Windows Terminal: keys
# sent to that pane reach the client as its terminal's input.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
OUTER="$TEST_TMUX -Ltestouter"
. "$(dirname "$0")/win32-wait.sh"
$TMUX kill-server 2>/dev/null
$OUTER kill-server 2>/dev/null
wait_gone

OUT=$(mktemp -d)
trap "$OUTER kill-server 2>/dev/null; $TMUX kill-server 2>/dev/null; rm -rf $OUT" 0 1 15
FAIL=0

fail() {
	echo "FAIL: $1"
	FAIL=1
}

CFG="$OUT/csi-u.conf"
cat >"$CFG" <<EOF
set -g default-shell "$(cygpath -m "$SYSTEMROOT")/System32/WindowsPowerShell/v1.0/powershell.exe"
set -g status off
set -s extended-keys on
set -s terminal-features[90] 'xterm-256color:extkeys'
bind -n Escape set -g @got Escape
bind -n M-[ set -g @got M-[
bind -n C-a set -g @got C-a
bind -n M-Escape set -g @got M-Escape
EOF

$TMUX -f "$CFG" new -d -x40 -y10 "Start-Sleep 60" </dev/null || exit 1
$OUTER -f "$CFG" new -d -x40 -y10 -e TERM=xterm-256color \
    "& '$(cygpath -ma "$TEST_TMUX")' -Ltest attach" </dev/null || exit 1

attached() {
	[ -n "$($TMUX list-clients 2>/dev/null)" ]
}
wait_until attached || { echo "client never attached"; $OUTER capture-pane -p; exit 1; }

got_is() {
	[ "$($TMUX show -gv @got 2>/dev/null)" = "$1" ]
}

# check NAME KEY HEX...: the bytes, as the client's terminal input, are KEY.
check() {
	_name=$1 _want=$2
	shift 2
	$TMUX set -g @got none
	$OUTER send-keys -H "$@"
	if wait_until got_is "$_want"; then
		echo "PASS: $_name"
	else
		fail "$_name read as $($TMUX show -gv @got)"
	fi
}

check "ESC [ 27 u is Escape" Escape 1b 5b 32 37 75
check "ESC [ 97 ; 5 u is still C-a" C-a 1b 5b 39 37 3b 35 75
check "ESC [ 27 ; 3 u is still M-Escape" M-Escape 1b 5b 32 37 3b 33 75

exit $FAIL
