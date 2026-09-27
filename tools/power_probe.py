"""Samples the game process CPU usage and NVIDIA GPU power/utilization while a run is active.
usage: power_probe.py <seconds> <start_delay_s> <out.txt>
CPU: only <GAME_NAME> processes whose executable lies inside this kit folder (the user may be
playing a release build at the same time). GPU power/utilization is the whole board's."""
import subprocess, sys, time
from kitcfg import GAME, ROOT
secs, delay, out = float(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
time.sleep(delay)
def cpu_seconds():
    r = subprocess.run(['powershell', '-NoProfile', '-Command',
                        f"(Get-Process {GAME} -ErrorAction SilentlyContinue | Where-Object {{ $_.Path -like '{ROOT}*' }} "
                        f"| Measure-Object -Property CPU -Sum).Sum"],
                       capture_output=True, text=True)
    try: return float(r.stdout.strip().replace(",", "."))
    except ValueError: return None
samples = []
c0, t0 = cpu_seconds(), time.time()
end = t0 + secs
while time.time() < end:
    r = subprocess.run(['nvidia-smi', '--query-gpu=power.draw,utilization.gpu', '--format=csv,noheader,nounits'],
                       capture_output=True, text=True)
    try:
        p, u = [float(x) for x in r.stdout.strip().split(',')]
        samples.append((p, u))
    except ValueError:
        pass
    time.sleep(0.5)
c1, t1 = cpu_seconds(), time.time()
import os
ncpu = os.cpu_count()
cpu = (c1 - c0) / (t1 - t0) * 100 if c0 is not None and c1 is not None else -1
pw = sum(s[0] for s in samples) / max(1, len(samples))
ut = sum(s[1] for s in samples) / max(1, len(samples))
line = f'cpu={cpu:.0f}% of one core ({cpu / ncpu:.1f}% of {ncpu} threads) gpu_power={pw:.1f}W gpu_util={ut:.0f}%'
print(line)
open(out, 'w').write(line + '\n')
