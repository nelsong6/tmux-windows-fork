# Waits for the win32 regression scripts, sourced after $TMUX is set. Each
# polls until its condition holds and fails after WAIT_TIMEOUT seconds, where
# a fixed sleep was slow when tmux was quick and failed when it was slow.

WAIT_TIMEOUT=${WAIT_TIMEOUT:-10}

# wait_until COMMAND [ARG ...]: until COMMAND succeeds.
wait_until() {
	_tries=$((WAIT_TIMEOUT * 10))
	until "$@"; do
		_tries=$((_tries - 1))
		[ $_tries -gt 0 ] || return 1
		sleep 0.1
	done
}

# pane_has TARGET PATTERN: a line on TARGET's screen matches grep PATTERN.
# Anchor output with ^: the typed command line holds the same text after
# its prompt.
pane_has() {
	$TMUX capture-pane -t"$1" -p 2>/dev/null | tr -d '\r' | grep -q -- "$2"
}

wait_for() {
	wait_until pane_has "$1" "$2"
}

# wait_ready TARGET: TARGET's shell has drawn its prompt, so typed input
# reaches it.
pane_ready() {
	$TMUX capture-pane -t"$1" -p 2>/dev/null | tr -d '\r' |
	    grep -v '^ *$' | tail -1 | grep -q '[$>#] *$'
}

wait_ready() {
	wait_until pane_ready "$1"
}

# wait_gone: the server on $TMUX's socket has exited.
server_gone() {
	! $TMUX ls >/dev/null 2>&1
}

wait_gone() {
	wait_until server_gone
}

# wait_panes TARGET COUNT: TARGET has COUNT panes.
pane_count_is() {
	[ "$($TMUX list-panes -t"$1" 2>/dev/null | wc -l)" -eq "$2" ]
}

wait_panes() {
	wait_until pane_count_is "$1" "$2"
}
