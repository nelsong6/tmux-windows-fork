#!/bin/sh

# A picker for palette.sh's tests, in place of fzt: the Unix twin of
# win32-palette-pick.ps1. It saves the rows it is given to DIR/rows, then
# follows DIR/pick, lines of name=value: it prints the id of the first row named
# pick (under the top-level row named under, when given), with a tab and text
# when text is given, and exits with status. DIR/done appears as it exits.

DIR=$1
cat >"$DIR/rows"
awk -F '\t' -v specfile="$DIR/pick" '
BEGIN {
	status = 0
	while ((getline line <specfile) > 0) {
		i = index(line, "=")
		spec[substr(line, 1, i - 1)] = substr(line, i + 1)
	}
	if ("status" in spec)
		status = spec["status"]
}
$1 == "0" { parent = $2 }
!done && $2 == spec["pick"] &&
    (!("under" in spec) || ($1 != "0" && parent == spec["under"])) {
	out = $5
	if ("text" in spec)
		out = out "\t" spec["text"]
	print out
	done = 1
}
END { exit status }
' "$DIR/rows"
status=$?
: >"$DIR/done"
exit $status
