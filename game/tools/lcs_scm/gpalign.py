# like offalign.py, for $gp-relative accesses: the reference port gp offset -> LCS gp offset at matching instructions
import collections, difflib, re, sys
LINE = re.compile(r"^\s*// 0x([0-9a-f]+): 0x[0-9a-f]+\s+(\S+)\s*(.*)$")
GP = re.compile(r"(-?0x[0-9A-Fa-f]+|-?\d+)\(\$gp\)")
def parse(p):
    out=[]
    for l in open(p,encoding='utf-8',errors='replace'):
        m=LINE.match(l)
        if m: out.append((int(m.group(1),16),m.group(2),m.group(3).split('#')[0].strip()))
    return out
def gp(ops):
    m=GP.search(ops)
    if not m: return None
    v=m.group(1); return int(v,16) if 'x' in v else int(v)
a,b=parse(sys.argv[1]),parse(sys.argv[2])
sm=difflib.SequenceMatcher(None,[x[1] for x in a],[x[1] for x in b],autojunk=False)
pairs=collections.defaultdict(collections.Counter)
for tag,i1,i2,j1,j2 in sm.get_opcodes():
    if tag!='equal': continue
    for k in range(i2-i1):
        ga,gb=gp(a[i1+k][2]),gp(b[j1+k][2])
        if ga is not None and gb is not None: pairs[ga][(gb,a[i1+k][1])]+=1
for o in sorted(pairs): print("the reference port gp%+#x -> " % o + ", ".join("LCS gp%+#x (%s) x%d" % (g,op,n) for (g,op),n in pairs[o].most_common(2)))
