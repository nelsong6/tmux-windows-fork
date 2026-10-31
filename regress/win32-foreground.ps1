# Press Win+` twice for regress/win32-global-foreground.sh: first with a
# window of this process in the foreground and a tmux client started from
# this process (so the window is its terminal's), then with that window
# hidden. Prints "fg=yes|no" (whether the window could be brought forward),
# "first=<value>" and "second=<value>", the @fg the binding set each time.
# SendInput lands on the desktop this runs on: CI only.
param([string]$Tmux, [string]$Socket, [string]$Session)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Fg {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit, Size = 40)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    static INPUT Key(ushort vk, uint flags) { var i = new INPUT(); i.type = 1; i.ki.vk = vk; i.ki.flags = flags; return i; }
    public static uint Send(ushort[] vks) {
        var keys = new INPUT[vks.Length * 2];
        for (int i = 0; i < vks.Length; i++) {
            keys[i] = Key(vks[i], 0);
            keys[keys.Length - 1 - i] = Key(vks[i], 2);
        }
        return SendInput((uint)keys.Length, keys, Marshal.SizeOf(typeof(INPUT)));
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
        [System.Windows.Forms.Application]::DoEvents()
        $v = (T show -gv '@fg') -join ''
        if ($v -ne '') { return $v }
        Start-Sleep -Milliseconds 50
    }
    return "(none, sent=$sent)"
}

$form = New-Object System.Windows.Forms.Form
$form.Text = 'fzt64-foreground'
$form.TopMost = $true
$form.Show()
[System.Windows.Forms.Application]::DoEvents()
# Windows lets a process take the foreground just after input it sent.
[Fg]::Send(@(0x12)) | Out-Null   # Alt
[Fg]::SetForegroundWindow($form.Handle) | Out-Null
$form.Activate()
[System.Windows.Forms.Application]::DoEvents()

$psi = New-Object System.Diagnostics.ProcessStartInfo $Tmux
$psi.Arguments = "-L$Socket -C attach -t $Session"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$client = [System.Diagnostics.Process]::Start($psi)
$client.BeginOutputReadLine()
$client.BeginErrorReadLine()
try {
$clock = [System.Diagnostics.Stopwatch]::StartNew()
while ($clock.Elapsed.TotalSeconds -lt 10) {
    if ((T list-clients -F '#{client_pid}') -contains "$($client.Id)") { break }
    Start-Sleep -Milliseconds 100
}
"client=" + $client.Id + " listed=" + ((T list-clients -F '#{client_pid}') -join ',')
"fg=" + $(if ([Fg]::GetForegroundWindow() -eq $form.Handle) { 'yes' } else { 'no' })
"first=" + (Press-And-Read)

$form.Hide()
[System.Windows.Forms.Application]::DoEvents()
Start-Sleep -Milliseconds 200
"owner=" + [Fg]::Owner() + " self=" + $PID
"second=" + (Press-And-Read)

} finally {
    $client.StandardInput.Close()
    if (-not $client.WaitForExit(5000)) { $client.Kill() }
    $form.Close()
}
