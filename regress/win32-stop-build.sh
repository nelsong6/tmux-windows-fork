#!/bin/sh

# Survey (TMUX-35, throwaway): lists what is left from this checkout's build
# folder, labelled $1, then ends it.

build=$(cygpath -aw "$(dirname "$0")/../build")
powershell.exe -NoProfile -Command "
	\$p = Get-CimInstance Win32_Process | Where-Object { \$_.ExecutablePath -like '$build\*' }
	if (\$p) { \$p | ForEach-Object { 'LEFTOVER [$1] pid ' + \$_.ProcessId + ' parent ' + \$_.ParentProcessId + ': ' + \$_.CommandLine } }
	else { 'CLEAN [$1]' }
	\$p | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force -ErrorAction SilentlyContinue }"
exit 0
