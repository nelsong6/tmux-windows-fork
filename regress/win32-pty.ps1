# Run a command in a pseudo-console, as Windows Terminal does, and resize the
# pseudo-console whenever ResizeFile appears (it holds "COLS ROWS" and is
# deleted once applied). Exits when the command does.
param(
    [Parameter(Mandatory)][string]$Command,
    [Parameter(Mandatory)][int]$Cols,
    [Parameter(Mandatory)][int]$Rows,
    [Parameter(Mandatory)][string]$ResizeFile
)
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
public static class Pty {
    [StructLayout(LayoutKind.Sequential)] struct COORD { public short X, Y; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct STARTUPINFO {
        public int cb; public string r, d, t; public int x, y, xs, ys, xc, yc, fa, f; public short sw, r2; public IntPtr r3, i, o, e; }
    [StructLayout(LayoutKind.Sequential)] struct STARTUPINFOEX { public STARTUPINFO si; public IntPtr attr; }
    [StructLayout(LayoutKind.Sequential)] struct PI { public IntPtr hp, ht; public int pid, tid; }
    [DllImport("kernel32.dll")] static extern int CreatePseudoConsole(COORD s, IntPtr i, IntPtr o, uint f, out IntPtr pc);
    [DllImport("kernel32.dll")] static extern int ResizePseudoConsole(IntPtr pc, COORD s);
    [DllImport("kernel32.dll")] static extern void ClosePseudoConsole(IntPtr pc);
    [DllImport("kernel32.dll")] static extern bool CreatePipe(out IntPtr r, out IntPtr w, IntPtr a, int s);
    [DllImport("kernel32.dll")] static extern bool InitializeProcThreadAttributeList(IntPtr l, int c, int f, ref IntPtr s);
    [DllImport("kernel32.dll")] static extern bool UpdateProcThreadAttribute(IntPtr l, uint f, IntPtr a, IntPtr v, IntPtr s, IntPtr p, IntPtr r);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool CreateProcessW(string a, string c, IntPtr pa, IntPtr ta, bool ih, uint f, IntPtr env, string dir, ref STARTUPINFOEX si, out PI pi);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    static IntPtr pc, proc;
    public static void Start(string cmd, short cols, short rows) {
        IntPtr inR, inW, outR, outW;
        CreatePipe(out inR, out inW, IntPtr.Zero, 0);
        CreatePipe(out outR, out outW, IntPtr.Zero, 0);
        int hr = CreatePseudoConsole(new COORD { X = cols, Y = rows }, inR, outW, 0, out pc);
        if (hr != 0) throw new Exception("CreatePseudoConsole " + hr);
        // Drain the output so the command never blocks writing.
        var drain = new FileStream(new Microsoft.Win32.SafeHandles.SafeFileHandle(outR, true), FileAccess.Read);
        new Thread(() => { var b = new byte[4096]; try { while (drain.Read(b, 0, b.Length) > 0) { } } catch { } }) { IsBackground = true }.Start();
        IntPtr size = IntPtr.Zero;
        InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
        var si = new STARTUPINFOEX();
        si.si.cb = Marshal.SizeOf(typeof(STARTUPINFOEX));
        si.si.f = 0x100; // STARTF_USESTDHANDLES with none: the pseudo-console's, not ours
        si.attr = Marshal.AllocHGlobal(size);
        InitializeProcThreadAttributeList(si.attr, 1, 0, ref size);
        UpdateProcThreadAttribute(si.attr, 0, (IntPtr)0x20016, pc, (IntPtr)IntPtr.Size, IntPtr.Zero, IntPtr.Zero);
        PI pi;
        if (!CreateProcessW(null, cmd, IntPtr.Zero, IntPtr.Zero, false, 0x80000, IntPtr.Zero, null, ref si, out pi))
            throw new Exception("CreateProcess " + Marshal.GetLastWin32Error());
        proc = pi.hp;
    }
    public static bool Exited(uint ms) { return WaitForSingleObject(proc, ms) == 0; }
    public static void Resize(short cols, short rows) { ResizePseudoConsole(pc, new COORD { X = cols, Y = rows }); }
    public static void Close() { ClosePseudoConsole(pc); }
}
'@
[Pty]::Start($Command, $Cols, $Rows)
while (-not [Pty]::Exited(50)) {
    if (Test-Path $ResizeFile) {
        $c, $r = (Get-Content $ResizeFile -Raw).Trim() -split '\s+'
        [Pty]::Resize([int]$c, [int]$r)
        Remove-Item $ResizeFile
    }
}
[Pty]::Close()
