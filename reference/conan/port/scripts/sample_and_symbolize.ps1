param(
    [Parameter(Mandatory=$true)][int[]]$ThreadIds,
    [Parameter(Mandatory=$true)][int]$ProcessId
)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public class Sampler {
    const uint CONTEXT_CONTROL = 0x100001;
    const uint THREAD_ALL_ACCESS = 0x1FFFFF;
    const int CONTEXT_SIZE = 1232;
    const int RIP_OFFSET = 248;
    const int CONTEXTFLAGS_OFFSET = 48;

    [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenThread(uint access, bool inherit, uint tid);
    [DllImport("kernel32.dll")] public static extern uint SuspendThread(IntPtr h);
    [DllImport("kernel32.dll")] public static extern int ResumeThread(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool GetThreadContext(IntPtr h, IntPtr ctx);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern uint GetLastError();

    [DllImport("dbghelp.dll", SetLastError = true)] public static extern bool SymInitialize(IntPtr hProcess, string UserSearchPath, bool fInvadeProcess);
    [DllImport("dbghelp.dll")] public static extern uint SymSetOptions(uint opts);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct SYMBOL_INFO {
        public uint SizeOfStruct;
        public uint TypeIndex;
        public ulong Reserved0, Reserved1;
        public uint Index, Size;
        public ulong ModBase;
        public uint Flags;
        public ulong Value, Address;
        public uint Register, Scope, Tag;
        public uint NameLen, MaxNameLen;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] Name;
    }

    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern bool SymFromAddr(IntPtr hProcess, ulong Address, out ulong Displacement, ref SYMBOL_INFO Symbol);

    [StructLayout(LayoutKind.Sequential)]
    public struct IMAGEHLP_LINE64 {
        public uint SizeOfStruct;
        public IntPtr Key;
        public uint LineNumber;
        [MarshalAs(UnmanagedType.LPStr)] public string FileName;
        public ulong Address;
    }

    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern bool SymGetLineFromAddr64(IntPtr hProcess, ulong dwAddr, out uint pdwDisplacement, ref IMAGEHLP_LINE64 Line);

    public static string LastDebug = "";

    public static ulong SampleRip(int tid) {
        IntPtr h = OpenThread(THREAD_ALL_ACCESS, false, (uint)tid);
        if (h == IntPtr.Zero) { LastDebug = "OpenThread failed: " + GetLastError(); return 0; }
        // 32-byte align a buffer big enough for CONTEXT (must be 16-byte aligned).
        IntPtr raw = Marshal.AllocHGlobal(CONTEXT_SIZE + 32);
        try {
            long addr = raw.ToInt64();
            long aligned = (addr + 31) & ~31L;
            IntPtr ctxPtr = new IntPtr(aligned);
            for (int i = 0; i < CONTEXT_SIZE; i++) Marshal.WriteByte(ctxPtr, i, 0);
            Marshal.WriteInt32(ctxPtr, CONTEXTFLAGS_OFFSET, (int)CONTEXT_CONTROL);

            SuspendThread(h);
            bool ok = GetThreadContext(h, ctxPtr);
            uint err = GetLastError();
            ResumeThread(h);

            ulong rip = ok ? (ulong)Marshal.ReadInt64(ctxPtr, RIP_OFFSET) : 0;
            LastDebug = string.Format("getContextOk={0} err={1} rip=0x{2:X}", ok, err, rip);
            return rip;
        } finally {
            Marshal.FreeHGlobal(raw);
            CloseHandle(h);
        }
    }

    public static string Symbolize(IntPtr hProcess, ulong rip) {
        var symbol = new SYMBOL_INFO();
        symbol.SizeOfStruct = 88;
        symbol.MaxNameLen = 256;
        symbol.Name = new byte[256];
        ulong disp;
        bool gotSym = SymFromAddr(hProcess, rip, out disp, ref symbol);
        string name = gotSym ? Encoding.ASCII.GetString(symbol.Name).TrimEnd('\0') : ("<unknown, err=" + GetLastError() + ">");

        var line = new IMAGEHLP_LINE64();
        line.SizeOfStruct = (uint)Marshal.SizeOf(typeof(IMAGEHLP_LINE64));
        uint lineDisp;
        bool gotLine = SymGetLineFromAddr64(hProcess, rip, out lineDisp, ref line);
        string lineInfo = gotLine ? string.Format(" at {0}:{1}", line.FileName, line.LineNumber) : "";
        return string.Format("0x{0:X} -> {1}+0x{2:X}{3}", rip, name, disp, lineInfo);
    }
}
'@

$hProc = [Sampler]::OpenProcess(0x1FFFFF, $false, $ProcessId)
[Sampler]::SymSetOptions(0x42) | Out-Null
[Sampler]::SymInitialize($hProc, $null, $true) | Out-Null

foreach ($tid in $ThreadIds) {
    $rip = [Sampler]::SampleRip($tid)
    if ($rip -eq 0) {
        Write-Output ("thread {0} : failed to sample [{1}]" -f $tid, [Sampler]::LastDebug)
        continue
    }
    $sym = [Sampler]::Symbolize($hProc, $rip)
    Write-Output ("thread {0} : {1}" -f $tid, $sym)
}
