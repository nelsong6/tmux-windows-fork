# Simulate Win+` and report whether a low-level keyboard hook in this process
# saw it: "seen", or "unseen" with SendInput's count and the session, when the
# desktop delivers no simulated input (as a service's session doesn't).
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Press {
    delegate IntPtr Proc(int code, IntPtr wParam, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] struct KB { public uint vk, scan, flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit, Size = 40)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr h; public uint m; public IntPtr w, l; public uint t; public int x, y; }
    [DllImport("user32.dll")] static extern IntPtr SetWindowsHookEx(int id, Proc fn, IntPtr mod, uint tid);
    [DllImport("user32.dll")] static extern IntPtr CallNextHookEx(IntPtr h, int c, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern bool PeekMessage(out MSG m, IntPtr h, uint a, uint b, uint remove);
    [DllImport("kernel32.dll")] static extern IntPtr GetModuleHandle(string n);
    static Proc keep; static bool seen;
    static IntPtr Cb(int code, IntPtr w, IntPtr l) {
        if (code >= 0 && ((KB)Marshal.PtrToStructure(l, typeof(KB))).vk == 0xC0) seen = true;
        return CallNextHookEx(IntPtr.Zero, code, w, l);
    }
    static INPUT Key(ushort vk, uint flags) { var i = new INPUT(); i.type = 1; i.ki.vk = vk; i.ki.flags = flags; return i; }
    public static string Run() {
        keep = Cb;
        SetWindowsHookEx(13, keep, GetModuleHandle(null), 0);
        var keys = new[] { Key(0x5B, 0), Key(0xC0, 0), Key(0xC0, 2), Key(0x5B, 2) };
        uint sent = SendInput((uint)keys.Length, keys, Marshal.SizeOf(typeof(INPUT)));
        MSG m;
        for (int i = 0; i < 100 && !seen; i++) { PeekMessage(out m, IntPtr.Zero, 0, 0, 1); System.Threading.Thread.Sleep(10); }
        return seen ? "seen" : "unseen sent=" + sent + " session=" + System.Diagnostics.Process.GetCurrentProcess().SessionId;
    }
}
'@
[Press]::Run()
