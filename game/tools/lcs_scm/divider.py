# Frame dividers: a counter +c in per-frame code, compared with a small threshold within a few instructions and reset
# soon after ("every N frames do X" - VIC5's Leone bar, found by the user 2026-11-16). Reads out/*.txt.
import os, re, sys
sys.argv = [sys.argv[0]]
HERE = os.path.dirname(os.path.abspath(__file__))
import importlib.util, io, contextlib
spec = importlib.util.spec_from_file_location('survey', os.path.join(HERE, 'survey.py'))
S = importlib.util.module_from_spec(spec)
with contextlib.redirect_stdout(io.StringIO()):
    spec.loader.exec_module(S)
for key, ins in S.scripts.items():
    hot, _ = S.per_frame(key)
    for i, (o, op, n, a) in enumerate(ins):
        if o not in hot or not re.match(r'^ADD_VAL_TO_INT_(L)?VAR$', n) or len(a) < 2 or not S.CONST.match(a[1]):
            continue
        v = a[0]
        cmp_ = None
        for j in range(i + 1, min(i + 4, len(ins))):
            if ins[j][2].startswith('IS_') and v in ins[j][3]:
                cmp_ = ins[j]
                break
        if not cmp_:
            continue
        reset = None
        for j in range(i + 1, min(i + 10, len(ins))):
            if ins[j][2].startswith('SET_') and ins[j][3] and ins[j][3][0] == v and ins[j][3][-1] == '0':
                reset = ins[j]
                break
        if not reset:
            continue
        # what the divider drives: the next change between the compare and the reset
        drives = [x for x in ins[i + 1:ins.index(reset)] if re.match(r'^(ADD|SUB)_', x[2])]
        print('%s %-8s +%05X %s %s | %s %s | drives %s' % (key, S.names.get(key, key), o, n, ' '.join(a), cmp_[2], ' '.join(cmp_[3]),
                                                          '; '.join('+%05X %s %s' % (x[0], x[2], ' '.join(x[3])) for x in drives) or '-'))
