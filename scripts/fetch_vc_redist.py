"""Fetch the Visual C++ runtime DLLs (app-local copy for the release folder) without
installing anything: reads the official Visual Studio 2022 channel manifest (the same one
xwin uses), takes the newest x64 CRT redist package and extracts its DLLs.

    python scripts/fetch_vc_redist.py <out_dir>

Called by scripts/setup_toolchain.sh (Microsoft license accepted there). Output:
<out_dir>/{msvcp140.dll, msvcp140_atomic_wait.dll, vcruntime140.dll, vcruntime140_1.dll, ...}
+ VERSION.txt. The DLLs match or are newer than the CRT libs xwin unpacks from the same
manifest, which is what app-local deployment requires.
"""
import hashlib
import io
import json
import os
import re
import sys
import urllib.request
import zipfile

CHANNEL = 'https://aka.ms/vs/17/release/channel'
WANTED = {'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll',
          'msvcp140_codecvt_ids.dll', 'vcruntime140.dll', 'vcruntime140_1.dll',
          'concrt140.dll'}
REQUIRED = {'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'}


def get(url):
    with urllib.request.urlopen(url, timeout=300) as r:
        return r.read()


def version_key(v):
    return tuple(int(x) for x in re.findall(r'\d+', v))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    channel = json.loads(get(CHANNEL))
    item = next(i for i in channel['channelItems']
                if i['id'] == 'Microsoft.VisualStudio.Manifests.VisualStudio')
    manifest = json.loads(get(item['payloads'][0]['url']))

    # x64 release CRT redist: e.g. Microsoft.VC.14.44.17.14.CRT.Redist.X64.base or
    # Microsoft.VisualCpp.CRT.Redist.X64 (id scheme varies across manifest versions).
    pat = re.compile(r'^Microsoft\.(VC\.[\d.]+|VisualCpp)\.CRT\.Redist\.X64(\.base)?$', re.I)
    cands = [p for p in manifest['packages'] if pat.match(p['id']) and p.get('payloads')]
    if not cands:
        sys.exit('[vc_redist] ERROR: no x64 CRT redist package in the VS manifest')
    pkg = max(cands, key=lambda p: version_key(p['version']))
    print(f"[vc_redist] {pkg['id']} {pkg['version']}")

    os.makedirs(out, exist_ok=True)
    got = set()
    for pl in pkg['payloads']:
        data = get(pl['url'])
        if pl.get('sha256') and hashlib.sha256(data).hexdigest().lower() != pl['sha256'].lower():
            sys.exit(f"[vc_redist] ERROR: SHA-256 mismatch for {pl['fileName']}")
        try:
            z = zipfile.ZipFile(io.BytesIO(data))       # .vsix payloads are zip files
        except zipfile.BadZipFile:
            continue
        for name in z.namelist():
            base = name.rsplit('/', 1)[-1].lower()
            low = name.lower()
            if base in WANTED and 'onecore' not in low and 'debug' not in low \
                    and '/arm' not in low and '/x86/' not in low:
                with open(os.path.join(out, base), 'wb') as f:
                    f.write(z.read(name))
                got.add(base)
    missing = REQUIRED - got
    if missing:
        sys.exit(f'[vc_redist] ERROR: {sorted(missing)} not found in {pkg["id"]}')
    with open(os.path.join(out, 'VERSION.txt'), 'w') as f:
        f.write(f"{pkg['id']} {pkg['version']}\n")
    print(f'[vc_redist] {len(got)} DLLs -> {out}')


if __name__ == '__main__':
    main()
