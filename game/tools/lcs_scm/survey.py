# Per-frame meters in LCS's scripts (the the reference port project's game-internals.md 11f method): the instructions that change a
# variable by a fixed amount (or by TIMERA / TIMERB, or by another variable) inside a WAIT 0 loop or a subroutine such a
# loop calls, ranked by how likely they are gameplay meters. Reads out/*.txt (lcsdis.py).
import collections, glob, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LINE = re.compile(r'^([0-9A-F]{6}) ([0-9A-F]{4}) (NOT )?(\S+) ?(.*)$')


def load(path):
    out = []
    for l in open(path, encoding='utf-8'):
        m = LINE.match(l.rstrip('\n'))
        if m:
            out.append((int(m.group(1), 16), int(m.group(2), 16), m.group(4), m.group(5).split(' ') if m.group(5) else []))
    return out


def label(v, mission):
    """a jump argument -> (script, offset): negative = this mission, positive = main"""
    v = int(v)
    if mission and v < 0:
        return 'self', -v
    return 'main', v


scripts = {}
for f in sorted(glob.glob(os.path.join(HERE, 'out', '*.txt'))):
    name = os.path.basename(f)[:-4]
    scripts[name] = load(f)
names = {}
for k, ins in scripts.items():
    for _, _, n, a in ins:
        if n == 'SCRIPT_NAME':
            names[k] = a[0].strip("'")
            break

JUMPS = {'GOTO', 'GOTO_IF_FALSE', 'GOTO_IF_TRUE'}
CALLS = {'GOSUB', 'CALL', 'CALL_NOT'}


def per_frame(key):
    """offsets (in this script) of instructions run once a frame: in a WAIT 0 loop, or in a subroutine of this script
    called from one (one level of the call graph for main-script subroutines: they are marked separately)"""
    ins = scripts[key]
    mission = key != 'main'
    idx = {o: i for i, (o, _, _, _) in enumerate(ins)}
    hot = set()
    hot_subs = set()
    for i, (o, op, n, a) in enumerate(ins):
        if n in JUMPS and a:
            s, t = label(a[0], mission)
            if (s == 'self' or not mission) and t <= o and t in idx:
                body = range(idx[t], i + 1)
                if any(ins[j][2] == 'WAIT' and ins[j][3] and ins[j][3][0] in ('0', '1') for j in body):
                    for j in body:
                        hot.add(ins[j][0])
                        if ins[j][2] in CALLS:
                            arg = ins[j][3][3] if ins[j][2] != 'GOSUB' else ins[j][3][0]
                            s2, t2 = label(arg, mission)
                            hot_subs.add((s2 if mission else 'main', t2))
    # subroutine bodies: from the label to the first RETURN (subroutines of subroutines one more level)
    for _ in range(2):
        for s2, t2 in list(hot_subs):
            if (s2 == 'self' or (s2 == 'main' and not mission)) and t2 in idx:
                j = idx[t2]
                while j < len(ins) and ins[j][2] != 'RETURN':
                    hot.add(ins[j][0])
                    if ins[j][2] in CALLS:
                        arg = ins[j][3][3] if ins[j][2] != 'GOSUB' else ins[j][3][0]
                        hot_subs.add(label(arg, mission) if mission else ('main', int(arg)))
                    j += 1
    return hot, hot_subs


CONST = re.compile(r'^-?[0-9.e+-]+$')
hits = []
counters = collections.defaultdict(list)  # (script, var) -> compared values
meters = set()
for key, ins in scripts.items():
    for o, op, n, a in ins:
        if n.startswith('IS_INT') and len(a) == 2 and CONST.match(a[1]) and abs(float(a[1])) >= 20:
            counters[(key, a[0])].append(a[1])
        if 'ONSCREEN_COUNTER' in n and a:
            meters.add((key, a[0]))

for key, ins in scripts.items():
    hot, _ = per_frame(key)
    for o, op, n, a in ins:
        if o not in hot or len(a) < 2:
            continue
        if not re.match(r'^(ADD|SUB)_', n) or 'TIMED' in n:
            continue
        if not re.search(r'_(VAL|INT|FLOAT|LVAR|VAR)', n) or any(w in n for w in ('BLIP', 'SOUND', 'SCORE', 'EXPLOSION', 'PARTICLE', 'MARKER', 'SPHERE', 'ARMOUR', 'STUCK', 'SPRITE')):
            continue
        var, src = a[0], a[1]
        scope = key
        score = 0
        why = []
        if src in ('TIMERA', 'TIMERB'):
            score += 5; why.append('+=timer')
        if (scope, var) in meters:
            score += 4; why.append('meter')
        if 'FLOAT' in n and CONST.match(src):
            v = abs(float(src))
            score += 2 if v < 1 else 1; why.append('float const %s' % src)
        if 'INT' in n and CONST.match(src) and (scope, var) in counters:
            score += 2; why.append('counter vs %s' % ','.join(sorted(set(counters[(scope, var)]))[:3]))
        if not CONST.match(src) and src not in ('TIMERA', 'TIMERB') and 'FLOAT' in n and var.startswith('$'):
            score += 1; why.append('float accumulator')
        if score:
            hits.append((score, key, names.get(key, key), o, n, ' '.join(a), '; '.join(why)))

hits.sort(key=lambda h: (-h[0], h[1], h[3]))
with open(os.path.join(HERE, 'survey.txt'), 'w', encoding='utf-8') as f:
    for h in hits:
        f.write('%d %s %-8s +%05X %s %s  [%s]\n' % h)
print(len(hits), 'hits;', sum(1 for h in hits if h[0] >= 4), 'with score >= 4')
for h in hits[:60]:
    print('%d %s %-8s +%05X %s %s  [%s]' % h)
