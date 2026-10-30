# Publish this fork's CI build as `tmux` to \\NelsonNAS\tools (Nelson's home
# setup; see shell-config-profile-home claude\topics\home-tools.md).
# The version is `git describe` (e.g. v3.6a-win32.7-5-g75474886), so only
# committed, pushed code is published.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (git status --porcelain) { throw 'commit first: a published version has to match a commit' }
git fetch -q origin
if (git log --oneline '@{u}..HEAD') { throw 'push first: the published commit has to be on GitHub' }

$version = git describe --tags --always
$commit = git rev-parse HEAD
$repo = 'nelsong6/tmux-windows-fork'
$workflow = 'Windows CI'

# CI's build-and-test job makes the package. A squash merge of a PR whose branch was up to
# date (master requires it) has the PR's last commit's tree, so that PR run already built
# this commit and there's no need to wait for the run on master.
$run = $null
$head = gh api "repos/$repo/commits/$commit/pulls" --jq '.[0].head.sha'
if ($head -and (gh api "repos/$repo/git/commits/$head" --jq .tree.sha) -eq (git rev-parse "$commit^{tree}")) {
    $run = gh run list -R $repo -w $workflow -c $head -e pull_request --json databaseId --jq '.[0].databaseId'
}
foreach ($attempt in 1..30) {
    if ($run) { break }
    $run = gh run list -R $repo -w $workflow -c $commit -e push --json databaseId --jq '.[0].databaseId'
    if ($run) { break }
    Start-Sleep -Seconds 10
}
if (-not $run) { throw "no CI run for $commit on $repo" }
gh run watch $run -R $repo --exit-status --interval 30
if ($LASTEXITCODE -ne 0) { throw "CI failed: https://github.com/$repo/actions/runs/$run" }

$dir = Join-Path ([IO.Path]::GetTempPath()) "tmux-$version"
Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
gh run download $run -R $repo -n tmux-windows -D $dir
if ($LASTEXITCODE -ne 0) { throw 'downloading tmux-windows failed' }

publish-tool -Name tmux -Version $version -Path $dir `
    -Entry tmux.exe -Repo "https://github.com/$repo" -Commit $commit

# Every open tab moves onto the release in place, and takes the config again, which
# a new build can make valid: the home profile's sync-servers.ps1, which home-sync
# also runs. The release runs from the Drive copy once Synology Drive has it, as new
# tabs do.
$local = Join-Path $HOME "tools\tmux\$version\tmux.exe"
foreach ($attempt in 1..24) {
    if (Test-Path $local) { break }
    Start-Sleep -Seconds 5
}
foreach ($s in & \\NelsonNAS\files\shell-config-profile-home\tmux\sync-servers.ps1) {
    Write-Host "$($s.Server): $($s.Build); config $($s.Config)"
}
