# A picker for display-palette's tests, in place of fzt. It saves the rows it
# is given to Dir/rows, then follows Dir/pick, lines of name=value (UTF-8, so
# non-ASCII text survives): it prints the id of the first row named pick
# (under the top-level row named under, when given), with a tab and text when
# text is given, and exits with status. Dir/done appears as it exits.
param(
    [Parameter(Mandatory)][string]$Dir
)
$utf8 = [Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8
[Console]::OutputEncoding = $utf8

$rows = [Console]::In.ReadToEnd()
[IO.File]::WriteAllText("$Dir/rows", $rows, $utf8)

$spec = @{ status = '0' }
foreach ($line in [IO.File]::ReadAllLines("$Dir/pick", $utf8)) {
    $key, $value = $line -split '=', 2
    $spec[$key] = $value
}

$parent = $null
foreach ($row in $rows -split "`n") {
    $depth, $name, $desc, $hint, $id = $row -split "`t"
    if ($depth -eq '0') { $parent = $name }
    if ($name -ne $spec.pick) { continue }
    if ($spec.under -and ($depth -eq '0' -or $parent -ne $spec.under)) { continue }
    $out = $id.Trim()
    if ($spec.ContainsKey('text')) { $out += "`t$($spec.text)" }
    [Console]::Out.Write($out + "`n")
    break
}
[IO.File]::WriteAllText("$Dir/done", '')
exit [int]$spec.status
