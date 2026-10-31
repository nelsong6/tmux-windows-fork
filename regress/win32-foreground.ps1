# Press Win+` twice for regress/win32-global-foreground.sh: first with a
# window of this process in the foreground and a tmux client started from
# this process (so the window is its terminal's), then with that window
# hidden. Logs "fg=yes|no" (whether the window could be brought forward),
# "first=<value>" and "second=<value>", the @fg the binding set each time.
# SendInput lands on the desktop this runs on: CI only.
param([string]$Tmux, [string]$Socket, [string]$Session, [string]$Log)

# Each line goes straight to the log, so a run that is killed leaves where it
# had got to.
function Say([string]$s) { [System.IO.File]::AppendAllText($Log, "$s`n") }
Say "start"

# A plain Win32 window: Windows Forms hung on CI's desktop.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Fg {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit, Size = 40)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr h; public uint m; public IntPtr w, l; public uint t; public int x, y; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateWindowExW(uint ex, string cls, string name, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] static extern bool PeekMessage(out MSG m, IntPtr h, uint a, uint b, uint remove);
    [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG m);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref MSG m);
    static INPUT Key(ushort vk, uint flags) { var i = new INPUT(); i.type = 1; i.ki.vk = vk; i.ki.flags = flags; return i; }
    public static uint Send(ushort[] vks) {
        var keys = new INPUT[vks.Length * 2];
        for (int i = 0; i < vks.Length; i++) {
            keys[i] = Key(vks[i], 0);
            keys[keys.Length - 1 - i] = Key(vks[i], 2);
        }
        return SendInput((uint)keys.Length, keys, Marshal.SizeOf(typeof(INPUT)));
    }
    public static IntPtr Window() {
        // WS_OVERLAPPEDWINDOW | WS_VISIBLE, WS_EX_TOPMOST
        return CreateWindowExW(0x8, "STATIC", "fzt64-foreground", 0x10CF0000, 100, 100, 300, 200, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
    }
    public static void Pump() {
        MSG m;
        while (PeekMessage(out m, IntPtr.Zero, 0, 0, 1)) { TranslateMessage(ref m); DispatchMessage(ref m); }
    }
    public static uint Owner() {
        uint pid = 0;
        IntPtr h = GetForegroundWindow();
        if (h != IntPtr.Zero) GetWindowThreadProcessId(h, out pid);
        return pid;
    }
}
'@

function T { & $Tmux "-L$Socket" @args 2>$null }

function Press-And-Read {
    T set -gu '@fg' | Out-Null
    $sent = [Fg]::Send(@(0x5B, 0xC0))   # Win, `
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt 10) {
        [Fg]::Pump()
        $v = (T show -gv '@fg') -join ''
        if ($v -ne '') { return $v }
        Start-Sleep -Milliseconds 50
    }
    return "(none, sent=$sent)"
}

Say "types"
$win = [Fg]::Window()
Say ("window=" + $win)
[Fg]::Pump()
# Windows lets a process take the foreground just after input it sent.
[Fg]::Send(@(0x12)) | Out-Null   # Alt
Say "alt"
[Fg]::SetForegroundWindow($win) | Out-Null
[Fg]::Pump()
Say "raised"

$psi = New-Object System.Diagnostics.ProcessStartInfo $Tmux
$psi.Arguments = "-L$Socket -C attach -t $Session"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$client = [System.Diagnostics.Process]::Start($psi)
$client.BeginOutputReadLine()
$client.BeginErrorReadLine()
Say "started"
try {
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt 10) {
        if ((T list-clients -F '#{client_pid}') -contains "$($client.Id)") { break }
        Start-Sleep -Milliseconds 100
    }
    Say ("client=" + $client.Id + " listed=" + ((T list-clients -F '#{client_pid}') -join ','))
    Say ("fg=" + $(if ([Fg]::GetForegroundWindow() -eq $win) { 'yes' } else { 'no' }))
    Say ("first=" + (Press-And-Read))

    [Fg]::ShowWindow($win, 0) | Out-Null   # SW_HIDE
    [Fg]::Pump()
    Start-Sleep -Milliseconds 200
    Say ("owner=" + [Fg]::Owner() + " self=" + $PID)
    Say ("second=" + (Press-And-Read))
} finally {
    $client.StandardInput.Close()
    if (-not $client.WaitForExit(5000)) { $client.Kill() }
    [Fg]::DestroyWindow($win) | Out-Null
    Say "done"
}
