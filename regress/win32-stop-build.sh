#!/bin/sh

# Ends the tmux processes left by an earlier test: those running from this
# checkout's build folder, and no other. Ending tmux.exe by name also ends
# whatever live server the machine runs, and the steps that run this are
# copied into hand runs on desktops (HOMECFG-97).

build=$(cygpath -aw "$(dirname "$0")/../build")
powershell.exe -NoProfile -Command "
	Get-CimInstance Win32_Process -Filter \"Name='tmux.exe'\" |
		Where-Object { \$_.ExecutablePath -like '$build\\*' } |
		ForEach-Object { Stop-Process -Id \$_.ProcessId -Force -ErrorAction SilentlyContinue }"
exit 0
