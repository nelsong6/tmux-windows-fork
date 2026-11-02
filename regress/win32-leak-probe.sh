#!/bin/sh
# Temporary (TMUX-35): leaves a server running, to show win32-run.sh failing.
$TEST_TMUX -Ltmux35leak -f/dev/null new -d -s leak </dev/null
