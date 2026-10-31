# Claude Code Project Notes — tmux-windows

## MSYS2 / Git Bash Shell Quirks

### taskkill syntax
MSYS2 converts single-slash flags (`/F`) into file paths. Always use **double slashes**:
```bash
taskkill //F //IM tmux.exe
```
Never use `taskkill /F /IM` — MSYS2 mangles `/F` into `F:/`.

### Before rebuilding tmux.exe
Always kill all tmux processes first, or the linker fails with `LNK1104: cannot open file 'tmux.exe'`:
```bash
taskkill //F //IM tmux.exe 2>/dev/null; sleep 2
```

### MSYS2 path conversion
MSYS2 converts Unix-style paths passed to native Windows executables. This affects:
- Relative paths like `regress/conf/foo.conf` may become `C\:/src/tmux/regress/conf/foo.conf`
- The escaped colon (`C\:`) has a literal backslash before the colon
- Code checking `path[1] == ':'` for Windows drive letters must also check `path[1] == '\\' && path[2] == ':'`

### Test temp variables
Never use `TMP` as a shell variable name in test scripts — it shadows Windows `%TMP%` and causes `GetTempFileName()` ACCESS_DENIED errors in child processes. Use `OUT` instead.

## Build Commands

```bash
# Debug build (from Git Bash)
taskkill //F //IM tmux.exe 2>/dev/null; sleep 2
powershell.exe -NoProfile -Command "& { cmd.exe /c '\"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat\" x64 >nul 2>&1 && cmake --build C:\src\tmux\build --config Debug' }"

# Run tests
TEST_TMUX=./build/Debug/tmux.exe bash regress/win32-basic.sh
```

## Test Suite

CI runs 17 test scripts from `regress/`:
- `win32-basic.sh` — core session/window/pane operations
- `win32-claude-swarm.sh` — multi-session concurrent usage
- `win32-format-strings.sh` — `#{...}` format engine (171 assertions)
- `win32-conf-syntax.sh` — config file parsing (21 files)
- `win32-keys.sh` — key handling (13 tests, tests effects not raw codes)
- `win32-has-session.sh` — exit code validation
- `win32-file.sh` — `source-file`, `load-buffer` and `save-buffer` from a second client finish, keep the bytes, and fail with exit 1
- `win32-layout.sh` — pane dimension verification (7 tests)
- `win32-control-client.sh` — complex pane operations, and a `-C` client's replies, subscriptions and exit (9 tests)
- `win32-attach-size.sh` — a client attaches at its console's size when the console is resized between identify and attach (runs the client in a pseudo-console via `win32-pty.ps1`)
- `win32-wrap.sh` — a row a pane wrapped reaches the client's console whole (the client runs in a pane of a second server, whose copy shows what it drew)
- `win32-linesize.sh` — a double-width or double-height line (DECDWL, DECDHL) wraps at half the pane's width and DECSWL restores it, in the pane and as the client draws it (outer-server pattern as above). That the terminal shows the rows at double size is checked by eye in Windows Terminal (`window-shot`), since no capture reports a row's size
- `win32-palette.sh` — `display-palette`'s rows and what a pick runs; `win32-palette-pick.ps1` stands in for fzt, and keys reach the pseudo-console client with `send-keys -K`
- `win32-keep-alive.sh` — a session with `keep-alive-window` set opens that window when its last one goes by kill-window, the shell exiting or move-window, and still ends by kill-session or when the replacement's program exits within a second
- `win32-new-window-draw.sh` � `new-window -W` switches the client only once the new pane has drawn (not on ConPTY's setup alone), fails without switching when the program exits or the window is killed first, and a killed waiting command gives the wait up
- `win32-global-foreground.sh` � `#{global_key_terminal_foreground}` is 1 when the client's terminal (a window of a process it runs under) had the focus at a global key, else 0; `win32-foreground.ps1` presses Win+` in CI only
- `win32-upgrade.sh` — `upgrade-server` hands a server with an attached client (unfocused, its pane asking for focus reports) to another process without restarting its panes, waits while copy mode is open, carries on when the new build fails, and keeps the global environment as it was, less its handover variable `TMUX_UPGRADE`, in it and in new panes (6 tests; runs beside `conpty.dll`)

CI's `macos` job builds with tmux's own Unix build (`sh autogen.sh && ./configure --enable-utf8proc && make`, Homebrew's autoconf, automake, pkgconf, libevent and utf8proc) for the work Mac, and runs `regress/palette.sh` (the palette tests, client attached from a pane of a second server, `palette-pick.sh` for fzt) and `regress/hold.sh` (`switch-client -H`/`-R`). The macOS keyboard (held modifiers, the global table as registered hotkeys) is `osdep-darwin-keys.c`; the build is the `tmux-macos` artifact.

CI's `linux` job does the same on Ubuntu (apt's autoconf, automake, pkg-config, bison, libevent, ncurses and utf8proc) for Nelson's Linux machine, with the same two tests and `regress/upgrade.sh` (`upgrade-server` on Linux: the new build is started from the old server and inherits every pty and socket at the same descriptor number, so nothing restarts). `adopt-pane` takes a pane's pty from another server (`pidfd_getfd`), for moving a server that has no `upgrade-server` onto the fork (profile 2's `bin/tmux-move-server`). The Linux keyboard is `osdep-linux-keys.c`: it reads the keyboards' evdev devices, so it needs the user in the `input` group and works under X11 and Wayland alike; the build is the `tmux-linux` artifact.

They wait with `regress/win32-wait.sh` (poll until a condition holds, fail after 10 s), not fixed sleeps. Match a command's output with `^`: the typed command line holds the same text.

## Publishing

`publish.ps1` waits for the commit's CI run, downloads its `tmux-windows` package (Release `tmux.exe` with `conpty.dll` and `OpenConsole.exe`) and publishes it as `tmux` to `\\NelsonNAS\tools`, then runs the home profile's `tmux/sync-servers.ps1`, which moves every tab server and the spare (`wt*`) onto the release with `upgrade-server`, sources the config again on each server it moved, and prints each one's result. Run it after every merge.

## Winget Package Submission

Package: `arndawg.tmux-windows` in `microsoft/winget-pkgs`.

### How to submit a new version

1. **Sync the fork** before anything else:
   ```bash
   gh api repos/arndawg/winget-pkgs/merge-upstream -X POST -f branch=master
   ```

2. **Create a branch** from the synced master:
   ```bash
   MASTER_SHA=$(gh api repos/arndawg/winget-pkgs/git/refs/heads/master --jq '.object.sha')
   gh api repos/arndawg/winget-pkgs/git/refs -X POST \
     -f ref=refs/heads/arndawg.tmux-windows-VERSION \
     -f sha="$MASTER_SHA"
   ```

3. **Create files via the contents API** (one PUT per file). Do NOT use the git trees API — on a repo this large it produces phantom diffs with hundreds of unrelated files. The contents API creates clean per-file commits:
   ```bash
   CONTENT=$(printf '%s' "$YAML" | base64 -w0)
   gh api repos/arndawg/winget-pkgs/contents/manifests/a/arndawg/tmux-windows/VERSION/FILENAME \
     -X PUT -f message="Add manifest" -f content="$CONTENT" \
     -f branch=arndawg.tmux-windows-VERSION
   ```
   Three files needed: `arndawg.tmux-windows.installer.yaml`, `arndawg.tmux-windows.locale.en-US.yaml`, `arndawg.tmux-windows.yaml`.

4. **Verify** the branch diff before creating the PR:
   ```bash
   gh api repos/arndawg/winget-pkgs/compare/master...arndawg.tmux-windows-VERSION \
     --jq '{ahead_by: .ahead_by, files: [.files[].filename]}'
   ```
   Must show exactly 3 files and 3 commits ahead.

5. **Create the PR** with `gh pr create --repo microsoft/winget-pkgs`.

6. **Validation takes up to 3 hours.** Monitor with:
   ```bash
   gh pr view PRNUM --repo microsoft/winget-pkgs --json labels,comments \
     --jq '{labels: [.labels[].name], latest: .comments[-1].body[:200]}'
   ```
   Look for the `Validation-Completed` label. The Azure DevOps pipeline comment has a badge link.

### Manifest template reference

Copy from the previous version directory in `manifests/a/arndawg/tmux-windows/` — update `PackageVersion`, `InstallerUrl`, `InstallerSha256`, `ReleaseDate`, `ReleaseNotes`, and `ReleaseNotesUrl`.

### Manifest formatting

Do **not** add a trailing blank line to manifest YAML files. Earlier versions (3.6a-win32.1/3/4) had one, but from v3.6a-win32.6 onward we omit it.

## Known Quirks

- A second client reads and writes files for the server synchronously (`file.c`, `_WIN32`): libevent's bufferevent never fires on a file descriptor that isn't a socket, so `source-file`, `load-buffer` and `save-buffer` from a second client hung until HOMECFG-44. The read happens in the client, so a newer client fixes it against an older server
- Command-line arguments arrive in the ANSI code page (no `wmain`/UTF-8 manifest), so non-ASCII in argv is mangled: `display -p '#{s/✳/*/:pane_title}'` inserted `*` between every byte. Put non-ASCII formats in a config file
- `default-shell` with forward slashes (`C:/WINDOWS/system32/cmd.exe`) leaves the first pane dead and the server exits; use backslashes
- A server started from Git Bash passes ignoring Ctrl-C on to its panes (issue 6), so the tests can't exercise Ctrl-C
