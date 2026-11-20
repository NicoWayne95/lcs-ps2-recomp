# Damage / drain over time: GET_*_HEALTH v -> SUB/ADD constant -> SET_*_HEALTH v within a few instructions, inside per-frame
# code (survey.per_frame). Output on stdout.
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
        if o not in hot or not (n.startswith('GET_') and ('HEALTH' in n or 'AMMO' in n or 'MONEY' in n or 'SPEED' in n)) or not a:
            continue
        v = a[-1]
        for j in range(i + 1, min(i + 8, len(ins))):
            nj, aj = ins[j][2], ins[j][3]
            if re.match(r'^(ADD|SUB)_', nj) and aj and aj[0] == v and len(aj) > 1 and S.CONST.match(aj[1]):
                for k in range(j + 1, min(j + 6, len(ins))):
                    nk, ak = ins[k][2], ins[k][3]
                    if nk.startswith('SET_') and ak and v in ak:
                        print('%s %-8s +%05X %s -> +%05X %s %s -> %s %s' % (key, S.names.get(key, key), o, n, ins[j][0], nj, ' '.join(aj), nk, ' '.join(ak)))
                        break
                break
