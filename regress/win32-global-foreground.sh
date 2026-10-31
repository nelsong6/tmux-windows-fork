#!/bin/sh

# A global key's binding gets #{global_key_terminal_foreground}: 1 when the
# window in the foreground belongs to a process the client runs under (its
# terminal), else 0. The keys are simulated (win32-foreground.ps1), so they
# are pressed only in CI, which has a desktop of its own; on a developer's PC
# they would land on theirs.

PATH=/bin:/usr/bin
TERM=screen

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
SOCKET=fzt64test-foreground
TMUX="$TEST_TMUX -L$SOCKET"
. "$(dirname "$0")/win32-wait.sh"
$TMUX kill-server 2>/dev/null
wait_gone

OUT=$(mktemp)
trap "rm -f $OUT; $TMUX kill-server 2>/dev/null" 0 1 15
FAIL=0

fail() {
	echo "FAIL: $1"
	FAIL=1
}

$TMUX -fNUL new -d -s fg </dev/null || exit 1
$TMUX bind -T global 'W-`' set -gF @fg '#{global_key_terminal_foreground}' ||
    fail "bind"
# Outside a global key the format is empty.
[ -z "$($TMUX display -p '#{global_key_terminal_foreground}' | tr -d '\r')" ] ||
    fail "format set outside a global key"

if [ -n "$CI" ]; then
	PS="$(cygpath "$SYSTEMROOT")/System32/WindowsPowerShell/v1.0/powershell.exe"
	: >$OUT.raw
	"$PS" -NoProfile -ExecutionPolicy Bypass \
	    -File "$(cygpath -w "$(dirname "$0")/win32-foreground.ps1")" \
	    -Tmux "$(cygpath -w "$TEST_TMUX")" -Socket $SOCKET -Session fg \
	    -Log "$(cygpath -w $OUT.raw)" >$OUT.err 2>&1 </dev/null &
	P=$!
	WAIT_TIMEOUT=90 wait_until eval '! kill -0 $P 2>/dev/null' ||
	    { kill $P 2>/dev/null; fail "press script still running after 90s"; }
	tr -d '\r' <$OUT.raw >$OUT
	tr -d '\r' <$OUT.err
	rm -f $OUT.raw $OUT.err
	cat $OUT
	grep -qx 'first=[01]' $OUT || fail "first press: $(cat $OUT)"
	if grep -qx 'fg=yes' $OUT; then
		grep -qx 'first=1' $OUT ||
		    fail "terminal in the foreground gave $(grep first= $OUT)"
	else
		echo "NOTE: the test window couldn't take the foreground"
	fi
	if grep -q '^owner=\([0-9]*\) self=\1$' $OUT; then
		echo "NOTE: the test window kept the foreground when hidden"
	else
		grep -qx 'second=0' $OUT ||
		    fail "another window in the foreground gave $(grep second= $OUT)"
	fi
	[ $FAIL -eq 0 ] && echo "PASS: global_key_terminal_foreground"
else
	echo "PASS: global_key_terminal_foreground (bound; press simulated only in CI)"
fi

$TMUX kill-server 2>/dev/null
[ $FAIL -eq 0 ] && exit 0
exit 1
