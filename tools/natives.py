"""Which engine natives the Shrek 2 script code actually calls, by name.

iNative sits in the UFunction tail, so every native declaration in the script
packages gives an index to name mapping; the walker gives the indices in use.
"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, FUNC_NET, FUNC_NATIVE
from uscript import Walker

paths = sorted(sys.argv[1:])
idx2name, used, where = {}, collections.Counter(), collections.defaultdict(set)

for path in paths:
    p = Package(path)
    for e in p.exports:
        if p.classof(e) != 'Function':
            continue
        end = e['off'] + e['size']
        ff = struct.unpack_from('<I', p.b, end - 4)[0]
        back = 9 if (ff & FUNC_NET) else 7
        if ff & FUNC_NET:
            ff = struct.unpack_from('<I', p.b, end - 6)[0]
        if not (ff & FUNC_NATIVE):
            continue
        i = struct.unpack_from('<H', p.b, end - back)[0]
        if i:
            idx2name.setdefault(i, (e['name'], p.name))

for path in paths:
    p = Package(path)
    w = Walker(p)
    for e in p.exports:
        if p.classof(e) == 'Function':
            try:
                w.function(e)
            except Exception:
                pass
    used.update(w.natives)
    for i in w.natives:
        where[i].add(p.name.replace('.u', ''))

named = [(i, c) for i, c in used.items() if i in idx2name]
unnamed = [(i, c) for i, c in used.items() if i not in idx2name]
print(f"natives declared in packages:   {len(idx2name)}")
print(f"natives actually called:       {len(used)}  (named {len(named)}, unnamed {len(unnamed)})")

bypkg = collections.Counter(idx2name[i][1] for i, _ in named)
print("where the called ones are declared:", dict(bypkg))

gameonly = [i for i, _ in named
            if where[i] <= {'SHGame', 'KWGame', 'GamePlay', 'AmbientCreatures'}]
print(f"called only from game code:    {len(gameonly)}")

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'natives-used.txt')
with open(out, 'w') as f:
    for i, c in sorted(used.items()):
        nm, pk = idx2name.get(i, ('?', '?'))
        f.write(f"{i:5} {nm:34} {pk:14} calls={c:6} in={','.join(sorted(where[i]))}\n")
print('full list:', out)
print()
for i, c in sorted(named, key=lambda x: -x[1])[:25]:
    print(f"  {i:5} {idx2name[i][0]:30} {c:7} calls")
