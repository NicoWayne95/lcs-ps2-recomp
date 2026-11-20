# usage: python ctx.py <script> <hex offset> [before] [after]: the listing around an instruction
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
key, off = sys.argv[1], int(sys.argv[2], 16)
b = int(sys.argv[3]) if len(sys.argv) > 3 else 8
a = int(sys.argv[4]) if len(sys.argv) > 4 else 8
lines = open(os.path.join(HERE, 'out', key + '.txt'), encoding='utf-8').read().splitlines()
for i, l in enumerate(lines):
    if l[:6].strip() and int(l[:6], 16) == off:
        for j in range(max(0, i - b), min(len(lines), i + a + 1)):
            print(('>> ' if j == i else '   ') + lines[j])
        break
