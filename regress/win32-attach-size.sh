#!/bin/sh

# A client attaches at its console's size even when the console is resized
# after the client identified and before it attached (HOMECFG-36: a Windows
# Terminal tab that maximized while reattaching stayed at 89x24). The client
# runs in a pseudo-console, as in Windows Terminal; "run -d" holds it between
# identify and attach while the pseudo-console is resized.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
. "$(dirname "$0")/win32-wait.sh"
$TMUX kill-server 2>/dev/null
wait_gone

OUT=$(mktemp -d)
trap "$TMUX kill-server 2>/dev/null; rm -rf $OUT" 0 1 15

$TMUX -fNUL new -d -x100 -y30 </dev/null || exit 1

PS="$(cygpath "$SYSTEMROOT")/System32/WindowsPowerShell/v1.0/powershell.exe"
"$PS" -NoProfile -ExecutionPolicy Bypass \
    -File "$(cygpath -w "$(dirname "$0")/win32-pty.ps1")" \
    -Command "\"$(cygpath -wa "$TEST_TMUX")\" -Ltest set -g @identified 1 ; run -d 3 ; attach" \
    -Cols 89 -Rows 24 -ResizeFile "$(cygpath -w "$OUT/resize")" &
HOST=$!

identified() {
	[ "$($TMUX show -gv @identified 2>/dev/null | tr -d '\r')" = 1 ]
}
wait_until identified || { echo "client never identified"; exit 1; }

echo "150 40" >"$OUT/resize.tmp" && mv "$OUT/resize.tmp" "$OUT/resize"
resized() {
	[ ! -e "$OUT/resize" ]
}
wait_until resized || { echo "pseudo-console never resized"; exit 1; }

client_size() {
	$TMUX list-clients -F '#{client_width}x#{client_height}' 2>/dev/null |
	    tr -d '\r'
}
client_is() {
	[ "$(client_size)" = "$1" ]
}
if ! wait_until client_is 150x40; then
	echo "client attached at $(client_size), expected 150x40"
	exit 1
fi

$TMUX kill-server 2>/dev/null
wait $HOST

echo "ALL ATTACH-SIZE TESTS PASSED"
exit 0
