# Every on-screen counter (DISPLAY_ONSCREEN_COUNTER*) per script, the variables that feed it (copies / int<->float
# conversions, two levels), and every change of those variables inside per-frame code (survey.per_frame), with its
# operand. Output: meters.txt.
import os, re, sys
sys.argv = [sys.argv[0]]
HERE = os.path.dirname(os.path.abspath(__file__))
import importlib.util
spec = importlib.util.spec_from_file_location('survey', os.path.join(HERE, 'survey.py'))
S = importlib.util.module_from_spec(spec)
import io, contextlib
with contextlib.redirect_stdout(io.StringIO()):
    spec.loader.exec_module(S)

COPY = re.compile(r'^(SET_|CSET_)\w*(VAR|LVAR)\w*_TO_\w*(VAR|LVAR)')
CHANGE = re.compile(r'^(ADD|SUB|MULT|DIV)_\w*(VAR|LVAR)')
out = open(os.path.join(HERE, 'meters.txt'), 'w', encoding='utf-8')
for key, ins in S.scripts.items():
    meters = {}
    for o, op, n, a in ins:
        if 'ONSCREEN_COUNTER' in n and a:
            meters.setdefault(a[0], o)
    if not meters:
        continue
    feeds = set(meters)
    for _ in range(2):
        for o, op, n, a in ins:
            if COPY.match(n) and len(a) == 2 and a[0] in feeds and not S.CONST.match(a[1]):
                feeds.add(a[1])
    hot, _ = S.per_frame(key)
    out.write('== %s %s: counters %s; fed by %s\n' % (key, S.names.get(key, key), ' '.join(sorted(meters)), ' '.join(sorted(feeds - set(meters)))))
    for o, op, n, a in ins:
        if o in hot and len(a) >= 2 and a[0] in feeds and CHANGE.match(n) and 'TIMED' not in n:
            out.write('   +%05X %s %s\n' % (o, n, ' '.join(a)))
        elif o in hot and len(a) >= 2 and a[0] in feeds and 'TIMED' in n:
            out.write('   +%05X %s %s  (timed)\n' % (o, n, ' '.join(a)))
out.close()
print(open(os.path.join(HERE, 'meters.txt'), encoding='utf-8').read())
