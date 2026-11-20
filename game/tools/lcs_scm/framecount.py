# The per-frame meter pattern: inside one WAIT 0 loop a variable is changed by a constant (or by a variable rate) and
# compared with a constant threshold (ints: >= 30), and the loop does not set it outright. Includes the subroutines the
# loop calls (survey.per_frame) for the changes. Output: framecount.txt.
import os, re, sys, collections
sys.argv = [sys.argv[0]]
HERE = os.path.dirname(os.path.abspath(__file__))
import importlib.util, io, contextlib
spec = importlib.util.spec_from_file_location('survey', os.path.join(HERE, 'survey.py'))
S = importlib.util.module_from_spec(spec)
with contextlib.redirect_stdout(io.StringIO()):
    spec.loader.exec_module(S)

CHANGE = re.compile(r'^(ADD|SUB)_(VAL|INT|FLOAT)\w*(VAR|LVAR)\w*$')
SETS = re.compile(r'^(SET_|CSET_)\w*(VAR|LVAR)')
SKIP = ('TIMED',)
rows = {}
for key, ins in S.scripts.items():
    mission = key != 'main'
    idx = {o: i for i, (o, _, _, _) in enumerate(ins)}
    # subroutine bodies by label
    def body_of(label_off):
        if label_off not in idx:
            return []
        j = idx[label_off]
        r = []
        while j < len(ins) and ins[j][2] != 'RETURN' and len(r) < 400:
            r.append(j)
            j += 1
        return r
    for i, (o, op, n, a) in enumerate(ins):
        if n not in S.JUMPS or not a:
            continue
        s, t = S.label(a[0], mission)
        if not ((s == 'self' or not mission) and t <= o and t in idx):
            continue
        lo = idx[t]
        loop = list(range(lo, i + 1))
        if not any(ins[j][2] == 'WAIT' and ins[j][3] and ins[j][3][0] in ('0', '1') for j in loop):
            continue
        if len(loop) > 1500:
            continue
        code = list(loop)
        for j in loop:
            if ins[j][2] in S.CALLS and ins[j][3]:
                arg = ins[j][3][3] if ins[j][2] != 'GOSUB' else ins[j][3][0]
                s2, t2 = S.label(arg, mission)
                if s2 == 'self' or not mission:
                    code += body_of(t2)
        compared = collections.defaultdict(set)
        setin = set()
        for j in code:
            nj, aj = ins[j][2], ins[j][3]
            if nj.startswith('IS_') and len(aj) == 2:
                for x, y in ((aj[0], aj[1]), (aj[1], aj[0])):
                    if S.CONST.match(y) and not S.CONST.match(x):
                        compared[x].add(y)
            if SETS.match(nj) and aj:
                setin.add(aj[0])
        for j in code:
            oj, opj, nj, aj = ins[j]
            if not CHANGE.match(nj) or any(k in nj for k in SKIP) or len(aj) < 2:
                continue
            var, src = aj[0], aj[1]
            if var in setin or var not in compared:
                continue
            th = [float(x) for x in compared[var]]
            if 'INT' in nj and not 'FLOAT' in nj and max(abs(x) for x in th) < 30:
                continue
            if src in ('TIMERA', 'TIMERB'):
                tag = 'timer'
            elif S.CONST.match(src):
                tag = 'const'
            else:
                tag = 'var'
            rows[(key, oj)] = (key, S.names.get(key, key), oj, nj, ' '.join(aj), sorted(compared[var])[:5], tag, ins[lo][0])
with open(os.path.join(HERE, 'framecount.txt'), 'w', encoding='utf-8') as f:
    for k in sorted(rows):
        key, name, o, n, a, c, tag, lo = rows[k]
        f.write('%s %-8s +%05X %s %s  cmp=%s [%s] loop@%X\n' % (key, name, o, n, a, ','.join(c), tag, lo))
print(len(rows))
print(open(os.path.join(HERE, 'framecount.txt'), encoding='utf-8').read())
