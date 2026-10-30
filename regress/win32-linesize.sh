#!/bin/sh

# Double-width and double-height lines (DECDWL, DECDHL; HOMECFG-55, now
# TMUX-1): a pane line set double size holds half the pane's width, so text
# wraps at the half; DECSWL puts it back to full width. The client runs in a
# pane of a second server, which stands in for Windows Terminal; that pane
# shows the client drew each row's text whole.

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

CFG="$OUT/linesize.conf"
cat >"$CFG" <<EOF
set -g default-shell "$(cygpath -m "$SYSTEMROOT")/System32/WindowsPowerShell/v1.0/powershell.exe"
set -g status off
EOF

# Row 0: double width, 30 cells, wraps at 20 of the pane's 40.
# Row 2: double height (top half), 10 cells.
# Row 3: double width set and reset (DECSWL), 30 cells on one row.
# Row 4: normal, 30 cells on one row.
E='[string][char]27'
NL='[Environment]::NewLine'
PRINT="[Console]::Write($E + '#6' + ('W' * 30) + $NL + $E + '#3' + ('T' * 10) + $NL + $E + '#6' + $E + '#5' + ('S' * 30) + $NL + ('N' * 30))"
$TMUX -f "$CFG" new -d -x40 -y10 \
    "while (-not (Test-Path '$(cygpath -m "$OUT/go")')) { Start-Sleep -Milliseconds 50 }; $PRINT; Start-Sleep 60" \
    </dev/null || exit 1
$OUTER -f "$CFG" new -d -x40 -y10 "& '$(cygpath -ma "$TEST_TMUX")' -Ltest attach" \
    </dev/null || exit 1

attached() {
	[ -n "$($TMUX list-clients 2>/dev/null)" ]
}
wait_until attached || { echo "client never attached"; $OUTER capture-pane -p; exit 1; }

touch "$OUT/go"
W20=WWWWWWWWWWWWWWWWWWWW
EXPECTED="$(printf '%s\n%s\n%s\n%s\n%s' \
    "$W20" WWWWWWWWWW TTTTTTTTTT \
    SSSSSSSSSSSSSSSSSSSSSSSSSSSSSS NNNNNNNNNNNNNNNNNNNNNNNNNNNNNN)"
rows() {
	$1 capture-pane -p 2>/dev/null | tr -d '\r' | head -5
}
inner_has() {
	[ "$(rows "$TMUX")" = "$EXPECTED" ]
}
if ! wait_until inner_has; then
	echo "pane held:"
	rows "$TMUX"
	exit 1
fi
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

echo "ALL LINE SIZE TESTS PASSED"
exit 0
