# Strong candidates: a change by a constant (or by another variable) inside a WAIT 0 loop that no conditional jump
# between the loop's WAIT and it skips (it runs every pass), of a variable the script compares with a threshold
# (counters / meters), or a float constant of a variable that is not reset in the same pass. Output: strong.txt.
import os, re, sys, collections
sys.argv = [sys.argv[0]]
HERE = os.path.dirname(os.path.abspath(__file__))
import importlib.util, io, contextlib
spec = importlib.util.spec_from_file_location('survey', os.path.join(HERE, 'survey.py'))
S = importlib.util.module_from_spec(spec)
with contextlib.redirect_stdout(io.StringIO()):
    spec.loader.exec_module(S)

CHANGE = re.compile(r'^(ADD|SUB)_(VAL|INT|FLOAT)\w*(VAR|LVAR)\w*$')
SKIP = ('BLIP', 'SOUND', 'SCORE', 'EXPLOSION', 'PARTICLE', 'MARKER', 'SPHERE', 'ARMOUR', 'STUCK', 'SPRITE', 'TIMED')
SETS = re.compile(r'^(SET_|CSET_)\w*(VAR|LVAR)')
out = []
for key, ins in S.scripts.items():
    mission = key != 'main'
    idx = {o: i for i, (o, _, _, _) in enumerate(ins)}
    compared = collections.defaultdict(set)
    for o, op, n, a in ins:
        if n.startswith('IS_') and len(a) == 2:
            for x, y in ((a[0], a[1]), (a[1], a[0])):
                if S.CONST.match(y) and not S.CONST.match(x):
                    compared[x].add(y)
    for i, (o, op, n, a) in enumerate(ins):
        if n not in S.JUMPS or not a:
            continue
        s, t = S.label(a[0], mission)
        if not ((s == 'self' or not mission) and t <= o and t in idx):
            continue
        lo = idx[t]
        waits = [j for j in range(lo, i + 1) if ins[j][2] == 'WAIT' and ins[j][3] and ins[j][3][0] in ('0', '1')]
        if not waits:
            continue
        w = waits[0]
        # every pass runs from the loop's head through its WAIT and on: an instruction j runs every pass when no
        # conditional jump between the head and j jumps past j (forward) - straight-line code of the loop
        for j in range(lo, i + 1):
            oj, opj, nj, aj = ins[j]
            if not CHANGE.match(nj) or any(k in nj for k in SKIP) or len(aj) < 2:
                continue
            skipped = False
            for k in range(lo, j):
                nk, ak = ins[k][2], ins[k][3]
                if nk in ('GOTO_IF_FALSE', 'GOTO_IF_TRUE', 'GOTO') and ak:
                    s2, t2 = S.label(ak[0], mission)
                    if t2 > oj or t2 < ins[lo][0]:
                        skipped = True
                        break
            if skipped:
                continue
            var, src = aj[0], aj[1]
            reset = any(SETS.match(ins[k][2]) and ins[k][3] and ins[k][3][0] == var for k in range(lo, i + 1))
            if reset and not compared.get(var):
                continue
            kind = 'float' if 'FLOAT' in nj else 'int'
            out.append((key, S.names.get(key, key), oj, nj, ' '.join(aj), sorted(compared.get(var, []))[:4], reset))
seen = set()
with open(os.path.join(HERE, 'strong.txt'), 'w', encoding='utf-8') as f:
    for key, name, o, n, a, cmp_, reset in sorted(out):
        if (key, o) in seen:
            continue
        seen.add((key, o))
        f.write('%s %-8s +%05X %s %s  cmp=%s%s\n' % (key, name, o, n, a, ','.join(cmp_), ' (set in the loop)' if reset else ''))
print(len(seen))
print(open(os.path.join(HERE, 'strong.txt'), encoding='utf-8').read())
