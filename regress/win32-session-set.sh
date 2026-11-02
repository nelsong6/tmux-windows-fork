#!/bin/sh

# session-set: with @session-set-dir set, the server keeps live/<socket>.json
# current itself, records each destroyed window in closed-windows, and
# reopen-window rebuilds the newest one. Nothing is recorded when the server
# shuts down, and a server that ends with its last session is marked closed.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Ltest"
. "$(dirname "$0")/win32-wait.sh"
$TMUX kill-server 2>/dev/null
wait_gone

DIR=$(mktemp -d)
WDIR=$(cygpath -m "$DIR")
CFG=$(mktemp)
printf "set -g default-shell '%s'\n" "$COMSPEC" >$CFG
trap "$TMUX kill-server 2>/dev/null; rm -rf $DIR $CFG" 0 1 15

fail() {
	echo "FAIL: $1"
	$TMUX lsw -a -F '#{session_name}:#{window_index} #{window_name} #{window_panes}' 2>&1
	cat "$DIR"/live/*.json 2>&1
	cat "$DIR/closed-windows" 2>&1
	exit 1
}

# A pane that stays up: cmd.exe runs it, and {id} lands in a comment.
SYS=$(cygpath -w "$SYSTEMROOT")
LONG="$SYS\System32\PING.EXE -n 300 127.0.0.1 >nul"

$TMUX -f"$(cygpath -m $CFG)" new -d -s main -n one || fail "start server"
SNAP="$DIR/live/$(basename "$($TMUX display -p '#{socket_path}' | tr -d '\r')").json"
$TMUX set -g @session-set-dir "$WDIR" || fail "set dir"
$TMUX set -g @session-set-resume "$LONG & rem resumed {id}" || fail "set resume"

snap_has() { grep -q -- "$1" "$SNAP" 2>/dev/null; }
closed_lines() { [ -f "$DIR/closed-windows" ] && wc -l <"$DIR/closed-windows" | tr -d ' ' || echo 0; }
has_window() {
	$TMUX lsw -t"=$1" -F '#{window_name}' 2>/dev/null | tr -d '\r' |
	    grep -qx "$2"
}
windows_are() {
	[ "$($TMUX lsw -t"=$1" -F '#{window_name}' 2>/dev/null | tr -d '\r' |
	    tr '\n' ' ' | sed 's/ $//')" = "$2" ]
}

# 1. The server writes its snapshot itself.
wait_until snap_has '"name":"main"' || fail "no snapshot written"
snap_has '"closedAt":null' || fail "a live snapshot is marked closed"

# 2. Nothing changes, nothing is written.
before=$(cat "$SNAP")
sleep 1.5
[ "$(cat "$SNAP")" = "$before" ] || fail "snapshot rewritten with nothing changed"

# 3. A window with a split and a Claude pane, then a third window.
$TMUX new-window -d -t main: -n two "$LONG" || fail "new-window two"
$TMUX split-window -d -t main:two "$LONG" || fail "split two"
$TMUX new-window -d -t main: -n three "$LONG" || fail "new-window three"
$TMUX set -p -t main:two.1 @claude-session abc123 || fail "set claude"
$TMUX set -p -t main:two.1 @claude-cwd 'C:\Windows' || fail "set claude cwd"
$TMUX select-pane -t main:two.1
wait_until snap_has '"claude":"abc123"' || fail "Claude session not in snapshot"
snap_has '"cwd":"C:/Windows"' || fail "Claude folder not in snapshot"
sizes() { $TMUX lsp -t "$1" -F '#{pane_width}x#{pane_height}' | tr -d '\r' | tr '\n' ' '; }
layout=$(sizes main:two)

# 4. Killed, recorded, reopened at its place.
$TMUX kill-window -t main:two || fail "kill-window"
wait_until test "$(closed_lines)" -eq 1 || fail "killed window not recorded"
$TMUX reopen-window -d || fail "reopen-window"
wait_until windows_are main "one two three" || fail "reopened window not at its place"
[ "$($TMUX display -p -t main:two '#{window_panes}' | tr -d '\r')" = 2 ] ||
    fail "reopened window lacks its split"
[ "$(sizes main:two)" = "$layout" ] ||
    fail "reopened window's layout differs"
$TMUX display -p -t main:two.1 '#{pane_start_command}' | grep -q 'resumed abc123' ||
    fail "Claude pane not resumed"
[ "$($TMUX display -p -t main:two '#{pane_index}' | tr -d '\r')" = 1 ] ||
    fail "active pane not restored"
[ "$(closed_lines)" -eq 0 ] || fail "reopened window still recorded"

# 5. Nothing left to reopen.
$TMUX reopen-window -d 2>/dev/null && fail "reopen-window with nothing closed succeeded"

# 6. A moved window is not closed.
$TMUX new -d -s other -n o1 "$LONG" || fail "new session other"
$TMUX move-window -s main:three -t other: || fail "move-window"
sleep 0.5
[ "$(closed_lines)" -eq 0 ] || fail "moved window recorded as closed"

# 7. A killed session's windows are recorded; reopening makes the session again.
$TMUX kill-session -t other || fail "kill-session"
wait_until test "$(closed_lines)" -eq 2 || fail "killed session's windows not recorded"
$TMUX reopen-window -d || fail "reopen first of other"
$TMUX reopen-window -d || fail "reopen second of other"
wait_until windows_are other "o1 three" || fail "session not made again with its windows"

# 8. The at menu's window is not recorded.
cp "$COMSPEC" "$DIR/fzt-automate.exe"
$TMUX new-window -d -t main: -n menu "$(cygpath -w "$DIR/fzt-automate.exe") /k" ||
    fail "menu window"
wait_until snap_has '"command":"fzt-automate"' || fail "menu program not seen"
$TMUX kill-window -t main:menu
sleep 0.5
[ "$(closed_lines)" -eq 0 ] || fail "menu window recorded"

# 9. A folder the pane reports (OSC 7) reaches the snapshot.
$TMUX new-window -d -t main: -n osc "$SYS\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -Command \"[Console]::Write([char]27+']7;file://h/C:/Users'+[char]7); Start-Sleep 300\"" ||
    fail "osc window"
wait_until snap_has '"cwd":"C:/Users"' || fail "reported folder not in snapshot"
$TMUX kill-window -t main:osc

# 10. A program started in a pane reaches the snapshot with no tmux command
# after it, and its window reopens running @session-set-restart-<program>.
$TMUX set -g @session-set-restart-PING "$LONG & rem restarted" ||
    fail "set restart"
$TMUX new-window -d -t main: -n prog || fail "prog window"
$TMUX send-keys -t main:prog "$LONG" Enter
wait_until snap_has '"name":"prog",[^]]*"command":"PING"' ||
    fail "program started in a pane not seen"
: >"$DIR/closed-windows"
$TMUX kill-window -t main:prog
wait_until test "$(closed_lines)" -eq 1 || fail "prog window not recorded"
$TMUX reopen-window -d || fail "reopen prog"
wait_until has_window main prog || fail "prog window not reopened"
$TMUX display -p -t main:prog '#{pane_start_command}' | grep -q 'restarted' ||
    fail "program not restarted"
$TMUX kill-window -t main:prog

# 11. At most 25 closed windows are kept.
: >"$DIR/closed-windows"
i=0
while [ $i -lt 27 ]; do
	$TMUX new-window -d -t main: -n "w$i" "$LONG"
	sleep 0.1
	$TMUX kill-window -t "main:w$i"
	i=$((i + 1))
done
wait_until test "$(closed_lines)" -eq 25 || fail "closed-windows not capped at 25 ($(closed_lines))"
head -1 "$DIR/closed-windows" | grep -q '	w26	' || fail "newest closed window not first"

# 12. kill-server records nothing and leaves the snapshot open for restore.
: >"$DIR/closed-windows"
$TMUX kill-server
wait_gone
[ "$(closed_lines)" -eq 0 ] || fail "kill-server recorded windows"
snap_has '"closedAt":null' || fail "killed server's snapshot marked closed"

# 13. A new server sets the earlier snapshot aside; one that ends with its last
# session is marked closed.
old=$(sed 's/.*"startTime":\([0-9]*\).*/\1/' "$SNAP")
sleep 1.1
$TMUX -f"$(cygpath -m $CFG)" new -d -s solo "$LONG" || fail "restart server"
$TMUX set -g @session-set-dir "$WDIR"
wait_until test -f "${SNAP%.json}-$old.json" || fail "earlier snapshot not set aside"
wait_until snap_has '"name":"solo"' || fail "new server's snapshot not written"
$TMUX kill-session -t solo
wait_gone
snap_has '"closedAt":null' && fail "server that ended with its last session not marked closed"

exit 0
