"""Per-thread CPU usage (with thread names) of a running process.
usage: thread_cpu.py <seconds> [process_name=<GAME_NAME>.exe | pid] [top=16]
Prints % of one core per thread over the interval and the process total.
Only a process whose executable lies inside this kit folder is measured (the user may be
running a release build with the same name); pass a PID to pick one explicitly."""
import ctypes
import os
import ctypes.wintypes as wt
import sys
import time

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10
from kitcfg import EXE, ROOT
pname = sys.argv[2] if len(sys.argv) > 2 else EXE
explicit_pid = int(pname) if pname.isdigit() else None
top = int(sys.argv[3]) if len(sys.argv) > 3 else 16
k32 = ctypes.windll.kernel32


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ThreadID', wt.DWORD),
                ('th32OwnerProcessID', wt.DWORD), ('tpBasePri', wt.LONG), ('tpDeltaPri', wt.LONG),
                ('dwFlags', wt.DWORD)]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.c_void_p), ('th32ModuleID', wt.DWORD),
                ('cntThreads', wt.DWORD), ('th32ParentProcessID', wt.DWORD),
                ('pcPriClassBase', wt.LONG), ('dwFlags', wt.DWORD), ('szExeFile', wt.WCHAR * 260)]


def image_path(pid):
    h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return ''
    buf = ctypes.create_unicode_buffer(1024)
    size = wt.DWORD(len(buf))
    ok = k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size))
    k32.CloseHandle(h)
    return buf.value if ok else ''


def find_pid():
    if explicit_pid is not None:
        return explicit_pid
    kit = os.path.normcase(os.path.abspath(ROOT))
    snap = k32.CreateToolhelp32Snapshot(2, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    mine, others = [], []
    while ok:
        if e.szExeFile.lower() == pname.lower():
            path = os.path.normcase(image_path(e.th32ProcessID))
            (mine if path.startswith(kit) else others).append(e.th32ProcessID)
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    if others:
        print(f'[thread_cpu] ignoring {pname} outside the kit (pids {others})', file=sys.stderr)
    if len(mine) > 1:
        sys.exit(f'[thread_cpu] several {pname} inside the kit (pids {mine}): pass the pid')
    return mine[0] if mine else None


def threads(pid):
    snap = k32.CreateToolhelp32Snapshot(4, 0)
    e = THREADENTRY32()
    e.dwSize = ctypes.sizeof(e)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid:
            out.append(e.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


k32.OpenThread.restype = wt.HANDLE
k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]


def sample(pid):
    res = {}
    for tid in threads(pid):
        h = k32.OpenThread(0x0800 | 0x0040, False, tid)  # QUERY_LIMITED_INFORMATION | QUERY_INFORMATION
        if not h:
            continue
        c, e, kt, ut = (wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME())
        name = ''
        if k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut)):
            t = ((kt.dwHighDateTime << 32) | kt.dwLowDateTime) + ((ut.dwHighDateTime << 32) | ut.dwLowDateTime)
            p = ctypes.c_wchar_p()
            if k32.GetThreadDescription(h, ctypes.byref(p)) >= 0 and p.value:
                name = p.value
            res[tid] = (t, name)
        k32.CloseHandle(h)
    return res


pid = find_pid()
if not pid:
    print('process not found')
    sys.exit(1)
a = sample(pid)
t0 = time.time()
time.sleep(secs)
b = sample(pid)
dt = time.time() - t0
rows = []
for tid, (t, name) in b.items():
    d = t - a.get(tid, (0, ''))[0]
    rows.append((d / 1e7 / dt * 100, tid, name))
rows.sort(reverse=True)
print(f'total {sum(r[0] for r in rows):.1f}% of one core')
for pct, tid, name in rows[:top]:
    print(f'{pct:6.1f}%  {tid:6d}  {name}')
