#!/bin/sh

# A row a pane let wrap on its own reaches the client's console whole: its
# last column and the wrapped part on the next row (HOMECFG-21: with the
# console's autowrap off, both were lost and later rows drew one row off).
# The client runs in a pane of a second server, which stands in for Windows
# Terminal; that pane shows what the client's console drew.

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

CFG="$OUT/wrap.conf"
cat >"$CFG" <<EOF
set -g default-shell "$(cygpath -m "$SYSTEMROOT")/System32/WindowsPowerShell/v1.0/powershell.exe"
set -g status off
EOF

# 40 columns, then more: the pane wraps it, as a program's long line.
$TMUX -f "$CFG" new -d -x40 -y10 \
    "while (-not (Test-Path '$(cygpath -m "$OUT/go")')) { Start-Sleep -Milliseconds 50 }; [Console]::Write(('A' * 39) + 'Z' + 'wrapped'); Start-Sleep 60" \
    </dev/null || exit 1
$OUTER -f "$CFG" new -d -x40 -y10 "& '$(cygpath -ma "$TEST_TMUX")' -Ltest attach" \
    </dev/null || exit 1

attached() {
	[ -n "$($TMUX list-clients 2>/dev/null)" ]
}
wait_until attached || { echo "client never attached"; $OUTER capture-pane -p; exit 1; }

# Written while the client is attached, so it is drawn as it arrives.
touch "$OUT/go"
EXPECTED="$(printf 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAZ\nwrapped')"
rows() {
	$1 capture-pane -p 2>/dev/null | tr -d '\r' | head -2
}
inner_has() {
	[ "$(rows "$TMUX")" = "$EXPECTED" ]
}
wait_until inner_has || { echo "pane never held the wrapped row"; exit 1; }
drawn() {
	[ "$(rows "$OUTER")" = "$EXPECTED" ]
}
if ! wait_until drawn; then
	echo "client drew:"
	rows "$OUTER"
	exit 1
fi

$OUTER kill-server 2>/dev/null
$TMUX kill-server 2>/dev/null

echo "ALL WRAP TESTS PASSED"
exit 0
