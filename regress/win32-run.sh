#!/bin/sh

# win32-run.sh TEST: runs regress/TEST and fails it if it leaves anything
# running from this checkout's build folder, naming each process and then
# ending it. It fails without running TEST if something from the build folder
# is running already, and ends nothing then.

dir=$(dirname "$0")
build=$(cygpath -aw "$dir/../build")

# running SECONDS ACTION: lists the build folder's processes still running
# after up to SECONDS for them to exit; ACTION end then ends them.
running() {
	powershell.exe -NoProfile -Command "
		\$deadline = (Get-Date).AddSeconds($1)
		do {
			\$p = @(Get-CimInstance Win32_Process |
				Where-Object { \$_.ExecutablePath -like '$build\\*' })
			if (-not \$p) { break }
			Start-Sleep -Milliseconds 200
		} while ((Get-Date) -lt \$deadline)
		\$p | ForEach-Object {
			'pid ' + \$_.ProcessId + ': ' + \$_.CommandLine
			if ('$2' -eq 'end') {
				Stop-Process -Id \$_.ProcessId -Force -ErrorAction SilentlyContinue
			}
		}" | tr -d '\r'
}

# report MESSAGE LIST: prints MESSAGE and LIST, and in GitHub Actions makes
# each line an error annotation on the run.
report() {
	echo "FAIL: $1"
	echo "$2" | sed 's/^/  /'
	[ -n "$GITHUB_ACTIONS" ] && echo "$2" | sed "s/^/::error::$1 /"
}

left=$(running 0 keep)
if [ -n "$left" ]; then
	report "already running from the build folder before $1:" "$left"
	exit 1
fi

bash "$dir/$1"
status=$?

left=$(running 10 end)
if [ -n "$left" ]; then
	report "$1 left running:" "$left"
	[ $status -eq 0 ] && status=1
fi
exit $status
