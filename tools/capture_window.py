"""Capture a top-level window by class name to PNG (launcher screenshots).
usage: capture_window.py <exe> <window_class> <out.png> [--close] [exe args...]
Starts <exe>, waits for the window, captures it with PrintWindow, optionally
closes it (WM_CLOSE)."""
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time

from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))

exe, cls, out = sys.argv[1], sys.argv[2], sys.argv[3]
close = '--close' in sys.argv[4:]
open_combo = next((int(a.split('=')[1]) for a in sys.argv[4:] if a.startswith('--open-combo=')), 0)
args = [a for a in sys.argv[4:] if a != '--close' and not a.startswith('--open-combo=')]
proc = subprocess.Popen([exe] + args, cwd=None)
# Only a window of the process started here (never another copy of the game the user
# has open).
EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)


def find_own_window():
    found = []

    def cb(h, _):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(h, buf, 256)
        if pid.value == proc.pid and buf.value == cls:
            found.append(h)
            return False
        return True

    user32.EnumWindows(EnumProc(cb), 0)
    return found[0] if found else 0


hwnd = 0
for _ in range(200):
    hwnd = find_own_window()
    if hwnd:
        break
    time.sleep(0.05)
if not hwnd:
    print('window not found')
    proc.kill()
    sys.exit(1)
time.sleep(1.0)
if open_combo:
    # Drop down the combo box with this control id (CB_SHOWDROPDOWN) for the shot.
    user32.SendMessageW(user32.GetDlgItem(hwnd, open_combo), 0x014F, 1, 0)
    time.sleep(0.5)
rect = wt.RECT()
user32.GetWindowRect(hwnd, ctypes.byref(rect))
w, h = rect.right - rect.left, rect.bottom - rect.top
hdc = user32.GetWindowDC(hwnd)
mem = gdi32.CreateCompatibleDC(hdc)
bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
gdi32.SelectObject(mem, bmp)
user32.PrintWindow(hwnd, mem, 2)  # PW_RENDERFULLCONTENT


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', wt.DWORD), ('biWidth', wt.LONG), ('biHeight', wt.LONG),
                ('biPlanes', wt.WORD), ('biBitCount', wt.WORD), ('biCompression', wt.DWORD),
                ('biSizeImage', wt.DWORD), ('biXPelsPerMeter', wt.LONG),
                ('biYPelsPerMeter', wt.LONG), ('biClrUsed', wt.DWORD), ('biClrImportant', wt.DWORD)]


bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
buf = ctypes.create_string_buffer(w * h * 4)
gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
Image.frombuffer('RGBA', (w, h), buf, 'raw', 'BGRA', 0, 1).convert('RGB').save(out)
print(f'captured {w}x{h} -> {out}')
if close:
    user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
    try:
        proc.wait(10)
        print('exit code', proc.returncode)
    except subprocess.TimeoutExpired:
        proc.kill()
        print('killed')
