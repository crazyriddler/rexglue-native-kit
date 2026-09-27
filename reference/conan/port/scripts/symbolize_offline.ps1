param(
    [Parameter(Mandatory=$true)][string]$ExePath,
    [Parameter(Mandatory=$true)][UInt64[]]$Rvas
)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public class OfflineSym {
    [DllImport("dbghelp.dll", SetLastError = true)] public static extern bool SymInitialize(IntPtr hProcess, string UserSearchPath, bool fInvadeProcess);
    [DllImport("dbghelp.dll")] public static extern uint SymSetOptions(uint opts);
    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern ulong SymLoadModuleEx(IntPtr hProcess, IntPtr hFile, string ImageName, string ModuleName, ulong BaseOfDll, uint DllSize, IntPtr Data, uint Flags);

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

    public static string Symbolize(IntPtr fakeProcess, ulong fakeBase, ulong addr) {
        var symbol = new SYMBOL_INFO();
        symbol.SizeOfStruct = 88;
        symbol.MaxNameLen = 256;
        symbol.Name = new byte[256];
        ulong disp;
        bool gotSym = SymFromAddr(fakeProcess, addr, out disp, ref symbol);
        string name = gotSym ? Encoding.ASCII.GetString(symbol.Name).TrimEnd('\0') : "<unknown>";

        var line = new IMAGEHLP_LINE64();
        line.SizeOfStruct = (uint)Marshal.SizeOf(typeof(IMAGEHLP_LINE64));
        uint lineDisp;
        bool gotLine = SymGetLineFromAddr64(fakeProcess, addr, out lineDisp, ref line);
        string lineInfo = gotLine ? string.Format(" at {0}:{1}", line.FileName, line.LineNumber) : "";
        return string.Format("{0}+0x{1:X}{2}", name, disp, lineInfo);
    }
}
'@

# Use a synthetic "process" handle (our own process) purely as a namespace for
# dbghelp's symbol tables - SymLoadModuleEx does not require the module to
# actually be mapped there when we pass explicit file path + base + size.
$fakeProcess = [System.Diagnostics.Process]::GetCurrentProcess().Handle
[OfflineSym]::SymSetOptions(0x42) | Out-Null   # SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS
if (-not [OfflineSym]::SymInitialize($fakeProcess, $null, $false)) {
    Write-Output "SymInitialize failed"
    exit 1
}

$fakeBase = [UInt64]0x10000000000  # arbitrary non-conflicting base for the synthetic load
$fileInfo = Get-Item $ExePath
$size = [uint32]$fileInfo.Length
$loaded = [OfflineSym]::SymLoadModuleEx($fakeProcess, [IntPtr]::Zero, $ExePath, $null, $fakeBase, $size, [IntPtr]::Zero, 0)
if ($loaded -eq 0) {
    Write-Output ("SymLoadModuleEx failed for {0}" -f $ExePath)
    exit 1
}
Write-Output ("Loaded {0} at synthetic base 0x{1:X}" -f $ExePath, $loaded)

foreach ($rva in $Rvas) {
    $addr = $loaded + $rva
    $sym = [OfflineSym]::Symbolize($fakeProcess, $loaded, $addr)
    Write-Output ("RVA 0x{0:X} -> {1}" -f $rva, $sym)
}
