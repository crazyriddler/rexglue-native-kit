# M1 baseline profiling: reusable window-focus / input / screenshot helpers for
# driving the conan.exe Win32 window without a physical controller.
#
# Dot-source this file, then use:
#   Focus-Conan
#   Send-ConanKeys "{ENTER}" -Times 3 -DelayMs 1200
#   Grab-ConanScreenshot -Path "C:\...\out.png"
#
# Rationale for PrintWindow+PW_RENDERFULLCONTENT instead of CopyFromScreen:
# this is a D3D-flip-model window; GDI BitBlt/CopyFromScreen cannot see the
# presented surface reliably. See conan-port/docs/port_status.md "Known
# limitations".

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class ConanWin {
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    public static bool ForceForeground(IntPtr hWnd) {
        IntPtr fgWnd = GetForegroundWindow();
        uint fgThread = 0;
        if (fgWnd != IntPtr.Zero) GetWindowThreadProcessId(fgWnd, out fgThread);
        uint targetThread;
        GetWindowThreadProcessId(hWnd, out targetThread);
        uint curThread = GetCurrentThreadId();
        bool a1 = false, a2 = false;
        if (fgThread != 0 && fgThread != curThread) a1 = AttachThreadInput(curThread, fgThread, true);
        if (targetThread != 0 && targetThread != curThread) a2 = AttachThreadInput(curThread, targetThread, true);
        if (IsIconic(hWnd)) ShowWindow(hWnd, 9);
        BringWindowToTop(hWnd);
        bool result = SetForegroundWindow(hWnd);
        if (a1) AttachThreadInput(curThread, fgThread, false);
        if (a2) AttachThreadInput(curThread, targetThread, false);
        return result;
    }
}
"@

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class ConanMouse {
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, int dx, int dy, uint dwData, IntPtr dwExtraInfo);
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;
}
"@

function Click-ConanAt {
    param([Parameter(Mandatory=$true)][int]$X, [Parameter(Mandatory=$true)][int]$Y)
    $proc = Get-ConanProcess
    if (-not $proc) { Write-Output "conan process not found"; return $false }
    $pt = New-Object ConanMouse+POINT
    $pt.X = $X; $pt.Y = $Y
    [ConanMouse]::ClientToScreen($proc.MainWindowHandle, [ref]$pt) | Out-Null
    [ConanMouse]::SetCursorPos($pt.X, $pt.Y) | Out-Null
    Start-Sleep -Milliseconds 80
    [ConanMouse]::mouse_event([ConanMouse]::MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [ConanMouse]::mouse_event([ConanMouse]::MOUSEEVENTF_LEFTUP, 0, 0, 0, [IntPtr]::Zero)
    return $true
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class ConanKbd {
    [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, IntPtr dwExtraInfo);
    public const uint KEYEVENTF_KEYUP = 0x0002;
}
"@

function Hold-ConanKey {
    param([Parameter(Mandatory=$true)][byte]$VkCode, [int]$HoldMs = 800)
    Focus-Conan | Out-Null
    [ConanKbd]::keybd_event($VkCode, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds $HoldMs
    [ConanKbd]::keybd_event($VkCode, 0, [ConanKbd]::KEYEVENTF_KEYUP, [IntPtr]::Zero)
}

function Get-ConanProcess {
    Get-Process conan -ErrorAction SilentlyContinue
}

function Focus-Conan {
    $proc = Get-ConanProcess
    if (-not $proc) { Write-Output "conan process not found"; return $false }
    $ok = [ConanWin]::ForceForeground($proc.MainWindowHandle)
    Start-Sleep -Milliseconds 200
    return $ok
}

function Send-ConanKeys {
    param(
        [Parameter(Mandatory=$true)][string]$Keys,
        [int]$Times = 1,
        [int]$DelayMs = 900
    )
    for ($i = 0; $i -lt $Times; $i++) {
        Focus-Conan | Out-Null
        [System.Windows.Forms.SendKeys]::SendWait($Keys)
        Start-Sleep -Milliseconds $DelayMs
    }
}

function Grab-ConanScreenshot {
    param([Parameter(Mandatory=$true)][string]$Path)
    $proc = Get-ConanProcess
    if (-not $proc) { Write-Output "conan process not found"; return $false }
    $hwnd = $proc.MainWindowHandle
    $rect = New-Object ConanWin+RECT
    [ConanWin]::GetClientRect($hwnd, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { Write-Output "bad client rect $w x $h"; return $false }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc()
    $ok = [ConanWin]::PrintWindow($hwnd, $hdc, 2)  # PW_RENDERFULLCONTENT
    $gfx.ReleaseHdc($hdc)
    $gfx.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output "PrintWindow ok=$ok -> $Path ($w x $h)"
    return $ok
}
