"""For every non-library function that calls recovered D3D API functions, list which ones.
Output sorted by address -> the game's renderer layer map."""
import collections
from disdb import DB
from names import all_names
db = DB.load(); c = db.calls(); N = all_names()
LIB = [(0x822DD000, 0x822FA000), (0x8257C000, 0x82582100)]
inlib = lambda a: any(lo <= a < hi for lo, hi in LIB)
use = collections.defaultdict(collections.Counter)
for api, name in N.items():
    for s in c.get(api, []):
        f = db.func_of(s)
        if not inlib(f): use[f][name] += 1
for f in sorted(use):
    tot = sum(use[f].values())
    print('%08X callers=%d  %s' % (f, len(c.get(f, [])), ', '.join('%s x%d' % (k.replace('D3DDevice_', '').replace('SetRenderState_', 'RS.'), v) for k, v in use[f].most_common())))
