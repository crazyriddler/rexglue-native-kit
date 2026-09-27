"""Summarize a perf CSV (from --perf_log_csv) into windowed stats.
usage: bench_summary.py file.csv [window_s] [from_s] [to_s]"""
import csv, sys, statistics as st
path=sys.argv[1]; win=float(sys.argv[2]) if len(sys.argv)>2 else 10
a=float(sys.argv[3]) if len(sys.argv)>3 else 0; b=float(sys.argv[4]) if len(sys.argv)>4 else 1e9
rows=[r for r in csv.DictReader(open(path)) if a*1000<=float(r['t_ms'])<b*1000]
def pct(v,p): v=sorted(v); return v[min(len(v)-1,int(len(v)*p))]
def line(w,label):
    ft=[int(r['frame_time_us']) for r in w]
    f=lambda k: st.mean(int(r[k]) for r in w)
    print(f"{label} n={len(w):5d} ft={st.mean(ft)/1000:6.2f}ms p50={pct(ft,.5)/1000:6.2f} p95={pct(ft,.95)/1000:6.2f} p99={pct(ft,.99)/1000:6.2f} draws={f('draw_calls'):6.1f} cpbusy={f('cp_busy_us')/1000:5.2f} idle={f('cp_idle_us')/1000:5.2f} fence={f('fence_wait_us')/1000:5.2f} gpu={f('gpu_busy_us')/1000:5.2f} swap={f('swap_us')/1000:5.2f} subm={f('submissions'):.1f} res={f('resolves'):.1f} rts={f('render_target_switches'):.1f} bar={f('barriers'):.1f} pso={f('pipelines_created'):.2f}"+(f" wrm={f('cp_waitregmem_us')/1000:5.2f}ms/{f('cp_waitregmem_count'):.1f}" if 'cp_waitregmem_us' in w[0] else ""))
t0=float(rows[0]['t_ms'])/1000 if rows else 0
lo=a if a>0 else 0
while rows:
    w=[r for r in rows if lo*1000<=float(r['t_ms'])<(lo+win)*1000]
    if w: line(w,f"{lo:6.0f}s")
    lo+=win
    if lo*1000>float(rows[-1]['t_ms']): break
if rows: line(rows,"  TOTAL")
