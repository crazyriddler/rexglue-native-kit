param(
    [Parameter(Mandatory=$true)][int]$ThreadId,
    [Parameter(Mandatory=$true)][int]$ProcessId
)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public class ThreadSampler {
    [StructLayout(LayoutKind.Sequential)]
    public struct M128A { public ulong Low; public long High; }

    [StructLayout(LayoutKind.Sequential, Pack = 16)]
    public struct CONTEXT64 {
        public ulong P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
        public uint ContextFlags, MxCsr;
        public ushort SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
        public uint EFlags;
        public ulong Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
        public ulong Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
        public ulong R8, R9, R10, R11, R12, R13, R14, R15;
        public ulong Rip;
        // (FP/vector regs omitted - fixed-size padding to keep struct layout sane for CONTEXT_CONTROL|INTEGER)
        public ulong pad0, pad1, pad2, pad3, pad4, pad5, pad6, pad7;
        public ulong pad8, pad9, pad10, pad11, pad12, pad13, pad14, pad15;
        public ulong pad16, pad17, pad18, pad19, pad20, pad21, pad22, pad23;
        public ulong pad24, pad25, pad26, pad27, pad28, pad29, pad30, pad31;
        public ulong pad32, pad33, pad34, pad35, pad36, pad37, pad38, pad39;
        public ulong pad40, pad41, pad42, pad43, pad44, pad45, pad46, pad47;
        public ulong pad48, pad49, pad50, pad51, pad52, pad53, pad54, pad55;
        public ulong pad56, pad57, pad58, pad59, pad60, pad61, pad62, pad63;
        public ulong pad64, pad65, pad66, pad67, pad68, pad69, pad70, pad71;
        public ulong pad72, pad73, pad74, pad75, pad76, pad77, pad78, pad79;
    }

    const uint CONTEXT_FULL = 0x10000B;
    const uint THREAD_ALL_ACCESS = 0x1FFFFF;

    [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint access, bool inherit, uint tid);
    [DllImport("kernel32.dll")] public static extern uint SuspendThread(IntPtr h);
    [DllImport("kernel32.dll")] public static extern int ResumeThread(IntPtr h);
    [DllImport("kernel32.dll")] public static extern bool GetThreadContext(IntPtr h, ref CONTEXT64 ctx);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);

    public static ulong SampleRip(int tid) {
        IntPtr h = OpenThread(THREAD_ALL_ACCESS, false, (uint)tid);
        if (h == IntPtr.Zero) return 0;
        try {
            SuspendThread(h);
            CONTEXT64 ctx = new CONTEXT64();
            ctx.ContextFlags = CONTEXT_FULL;
            bool ok = GetThreadContext(h, ref ctx);
            ResumeThread(h);
            return ok ? ctx.Rip : 0;
        } finally {
            CloseHandle(h);
        }
    }
}
'@

$rip = [ThreadSampler]::SampleRip($ThreadId)
Write-Output ("0x{0:X}" -f $rip)
