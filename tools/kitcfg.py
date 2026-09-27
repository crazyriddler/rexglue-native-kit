"""Reads kit.env (KEY=VALUE, '#' comments) for the Python tools.

    from kitcfg import CFG, ROOT, PORT
    CFG['GAME_NAME'], CFG['TITLE_ID'] ...
Environment variables of the same name override the file.
"""
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))


def _load():
    cfg = {}
    path = os.path.join(ROOT, 'kit.env')
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            line = line.strip()
            if not line or line.startswith('#') or '=' not in line:
                continue
            k, v = line.split('=', 1)
            v = v.split(' #')[0].strip().strip('"')
            cfg[k.strip()] = v
    for k in list(cfg):
        cfg[k] = os.environ.get(k, cfg[k])
    return cfg


CFG = _load()
PORT = os.path.join(ROOT, CFG.get('PORT_DIR', 'port'))
GAME = CFG.get('GAME_NAME', 'game')
EXE = GAME + '.exe'


def win_path(p):
    """/c/Users/... (Git Bash) -> C:\\Users\\..."""
    if len(p) > 2 and p[0] == '/' and p[2] == '/':
        return p[1].upper() + ':' + p[2:].replace('/', '\\')
    return p


def kit_path(p):
    """kit.env path: relative = kit root, /c/... = Git Bash absolute."""
    if p and not (p.startswith('/') or (len(p) > 1 and p[1] == ':')):
        return os.path.join(ROOT, p)
    return win_path(p)


LLVM_SYMBOLIZER = os.path.join(kit_path(CFG.get('LLVM_DIR', '')), 'bin', 'llvm-symbolizer.exe')
