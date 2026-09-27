"""Synthetic self-test of tools/re/xdk_layout.py: rebuild a fake game layout from the
reference table (other code between objects, padding after functions) and check that
the draw/resolve/CRT entry points are recovered with the right names."""
import random
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "re"))

import xdk_layout  # noqa: E402

REF = REPO_ROOT / "tools" / "re" / "xdk_2012_dc3_symbols.tsv"
WANT = ["D3DDevice_DrawVertices", "D3DDevice_DrawIndexedVertices", "D3DDevice_Resolve",
        "D3DDevice_Swap", "D3DDevice_BeginTiling", "memcpy", "setjmp", "longjmp"]


def fake_game(pad):
    """Each library linked contiguously, ~25% of its objects not linked (never the ones
    holding WANT nor their direct neighbours: an isolated function needs linked
    neighbours to form a run, a documented limit of the method), game code of random
    sizes between libraries."""
    rng = random.Random(1)
    ref = xdk_layout.read_reference(REF, {"d3d9i", "LIBCMT"})
    addr, funcs, truth = xdk_layout.BASE, [], {}
    for _, seq in ref.items():
        for _ in range(rng.randint(5, 20)):  # unrelated game code before the library
            size = rng.randrange(8, 400, 4) + 2  # odd sizes never match the reference
            funcs.append((addr, size)); addr += size + 2
        objs = []
        for name, size, obj in seq:
            if not objs or objs[-1][0] != obj:
                objs.append((obj, []))
            objs[-1][1].append((name, size))
        wanted = {i for i, (_, fns) in enumerate(objs) if any(n in WANT for n, _ in fns)}
        near = {i + d for i in wanted for d in (-1, 0, 1)}
        for idx, (obj, fns) in enumerate(objs):
            keep = idx in near or rng.random() > 0.25
            if not keep:
                continue
            for name, size in fns:
                funcs.append((addr, size + (8 if pad else 0)))
                truth[addr] = name
                addr += size + (8 if pad else 0)
    return funcs, truth


def check(best, truth):
    got = {truth[a]: best[a][0] for a in best if a in truth}
    for name in WANT:
        assert got.get(name) == name, name
    wrong = sum(1 for a in best if a in truth and best[a][0] != truth[a])
    assert wrong / max(1, len(best)) < 0.05


def test_exact_sizes():
    funcs, truth = fake_game(pad=False)
    check(xdk_layout.match(funcs, xdk_layout.read_reference(REF, {"d3d9i", "LIBCMT"}), 3, exact=True), truth)


def test_padded_sizes():
    funcs, truth = fake_game(pad=True)
    check(xdk_layout.match(funcs, xdk_layout.read_reference(REF, {"d3d9i", "LIBCMT"}), 3, exact=False), truth)
