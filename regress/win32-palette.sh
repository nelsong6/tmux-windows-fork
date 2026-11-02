#!/bin/sh

# display-palette (FZT-54): the rows a picker is given, and what its pick runs
# once the popup closes. win32-palette-pick.ps1 stands in for fzt: it saves the
# rows and prints the id of the row each test names. The client runs in a
# pseudo-console, as in Windows Terminal, and keys reach it with send-keys -K.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
. "$(dirname "$0")/win32-wait.sh"
$TMUX kill-server 2>/dev/null
wait_gone

OUT=$(mktemp -d)
trap "$TMUX kill-server 2>/dev/null; rm -rf $OUT" 0 1 15

PS="$(cygpath -m "$SYSTEMROOT")/System32/WindowsPowerShell/v1.0/powershell.exe"
PICK="$(cygpath -m "$(dirname "$0")/win32-palette-pick.ps1")"

cat >"$OUT/test.conf" <<EOF
set -g default-shell "$PS"
set -g status off
bind -n F5 display-palette -w 100% -h 100% "$PS" -NoProfile -ExecutionPolicy Bypass -File "$PICK" -Dir "$(cygpath -m "$OUT")"
bind -N 'Rename the window' n command-prompt -I'#W' { rename-window -- '%%' }
bind -n M-w display-menu Split s { split-window } '' "#{?#{==:1,0},Never,}" x { kill-server } Last l { last-window }
set -g @go-menu 'display-menu -T go "#{session_name}" g "select-window -t first"'
set -s command-alias[100] 'go-to=run-shell -C "#{E:@go-menu}"'
set -g @no-menu 'display-menu -T none #{?#{==:1,0},"Never" n "kill-server",}'
set -s command-alias[101] 'go-nowhere=run-shell -C "#{E:@no-menu}"'
set -s command-alias[102] 'clock-mode=display-message clock'
EOF
$TMUX -f "$(cygpath -m "$OUT/test.conf")" new -d -x100 -y30 -n first \
    </dev/null || exit 1
$TMUX new-window -d -n second || exit 1

"$PS" -NoProfile -ExecutionPolicy Bypass \
    -File "$(cygpath -w "$(dirname "$0")/win32-pty.ps1")" \
    -Command "\"$(cygpath -wa "$TEST_TMUX")\" -Ltest attach" \
    -Cols 100 -Rows 30 -ResizeFile "$(cygpath -w "$OUT/resize")" &
HOST=$!
attached() {
	[ -n "$($TMUX list-clients 2>/dev/null)" ]
}
wait_until attached || { echo "client never attached"; exit 1; }
CLIENT=$($TMUX list-clients -F '#{client_name}' | tr -d '\r')

# palette LINE ...: open the palette with the picker told LINE ... (name=value),
# and wait for the picker to exit.
picker_done() {
	[ -e "$OUT/done" ]
}
palette() {
	rm -f "$OUT/rows" "$OUT/done"
	printf '%s\n' "$@" >"$OUT/pick"
	$TMUX send-keys -K -c "$CLIENT" F5
	wait_until picker_done || { echo "picker never ran ($*)"; exit 1; }
}

window_count() {
	$TMUX list-windows 2>/dev/null | wc -l
}
count_is() {
	[ "$(window_count)" -eq "$1" ]
}
window_name() {
	$TMUX display -p '#{window_name}' 2>/dev/null | tr -d '\r'
}
window_name_is() {
	[ "$(window_name)" = "$1" ]
}
row() {
	tr -d '\r' <"$OUT/rows" | grep -q -- "$1"
}

# A command with no arguments runs as picked.
palette pick=new-window
wait_until count_is 3 || { echo "new-window did not run: $(window_count) windows"; exit 1; }
$TMUX kill-window
wait_until count_is 2 || exit 1
$TMUX select-window -t first

# The rows: a command with its description from the manual, an alias, a
# binding named by its note, a root binding's menu as a folder of the items it
# shows (Never is hidden by its format), and a chooser's sessions and windows.
fail_row() {
	echo "$1"
	tr -d '\r' <"$OUT/rows" | grep -- "$2"
	exit 1
}
row '^0	kill-pane	Destroy the given pane\.		[0-9]*	kill pane window$' ||
    fail_row "no kill-pane row" kill-pane
row '^0	splitp	alias for split-window		' || fail_row "no alias row" splitp
row '^0	Rename the window	command-prompt.*	new-name (now first)	' ||
    fail_row "no prompted binding row" Rename
row '^0	M-w	' || fail_row "no root binding row" M-w
row '^1	Split		' || fail_row "no menu item row" Split
row '^1	Split			[0-9]*	$' || fail_row "a menu item has tags" Split
row '^1	Never	' && fail_row "a hidden menu item was offered" Never
row '^0	choose-tree	' || fail_row "no choose-tree row" choose-tree
row '^2	1: second	1 panes	' || fail_row "no window under a session" second
# An alias that runs a menu through run-shell -C, as send-to-session does: its format
# is expanded and its menu is a folder (building it once crashed the server).
row '^0	go-to	alias for run-shell' || fail_row "no run-shell -C alias row" go-to
row '^1	0		' || fail_row "no item in the run-shell -C menu" "^1	0"
row "^0	go-nowhere	alias for run-shell.*		[0-9]*	run shell miscellaneous\$" ||
    fail_row "no row for a run-shell -C menu with nothing in it" go-nowhere
# An alias that takes a command's name replaces that command's row, as typing
# the name runs the alias.
[ "$(tr -d '\r' <"$OUT/rows" | grep -c '^0	clock-mode	')" = 1 ] &&
    row '^0	clock-mode	alias for display-message clock	' ||
    fail_row "clock-mode is not one alias row" clock-mode

# A pick that asks for text runs its template with the text, quotes and
# non-ASCII intact.
palette 'pick=Rename the window' 'text=pal "q" é'
wait_until window_name_is 'pal "q" é' ||
    { echo "rename got: $(window_name)"; exit 1; }

# A command that takes one argument is typed in the picker.
palette pick=rename-window 'text=two words'
wait_until window_name_is 'two words' ||
    { echo "rename-window got: $(window_name)"; exit 1; }

# A choice in a chooser's folder runs on the pressing client.
palette 'pick=1: second' under=choose-tree
wait_until window_name_is second ||
    { echo "choose-tree pick went to: $(window_name)"; exit 1; }

# A menu item runs.
palette pick=Split under=M-w
wait_panes : 2 || { echo "the menu item did not run"; exit 1; }

# A picker that exits non-zero (Escape in fzt) runs nothing.
palette pick=new-window status=130
sleep 0.5
count_is 2 || { echo "a cancelled pick ran"; exit 1; }

$TMUX kill-server 2>/dev/null
wait $HOST

echo "ALL PALETTE TESTS PASSED"
exit 0
