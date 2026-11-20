# LCS MAIN.SCM disassembler (analysis only, scratch). The argument encoding as the reference port's (game-internals.md 11c there):
# type byte + data; parameter counts from Sanny Builder's library (lcs.json).
# usage: python lcsdis.py <MAIN.SCM> <out dir>
import json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
J = json.load(open(os.path.join(HERE, 'lcs.json'), encoding='utf-8'))
CMDS = {}
for ext in J['extensions']:
    for c in ext['commands']:
        CMDS[int(c['id'], 16)] = c


class Desync(Exception):
    pass


def arg(d, p):
    """one argument at p -> (text, next p, kind)"""
    t = d[p]
    p += 1
    if t == 0:
        return 'END', p, 'end'
    if t in (1, 2):
        return ('0' if t == 1 else '0.0'), p, 'num'
    if t in (3, 4, 5):
        n = t - 2
        raw = b'\0' * (4 - n) + d[p:p + n]
        return '%g' % struct.unpack('<f', raw)[0], p + n, 'num'
    if t == 6:
        return str(struct.unpack_from('<i', d, p)[0]), p + 4, 'int'
    if t == 7:
        return str(struct.unpack_from('<b', d, p)[0]), p + 1, 'int'
    if t == 8:
        return str(struct.unpack_from('<h', d, p)[0]), p + 2, 'int'
    if t == 9:
        return '%g' % struct.unpack_from('<f', d, p)[0], p + 4, 'num'
    # LCS (found 2026-11-16 by decoding): one below the reference port - globals from 0xCC ('0622 cc 02' GET_MULTIPLAYER_MODE $G2)
    if t in (0x0A, 0x0B):  # LCS: the timers ('0019 0a 08 40 1f' IS_INT_LVAR_GREATER_THAN_NUMBER TIMERA 8000)
        return ('TIMERA' if t == 0x0A else 'TIMERB'), p, 'var'
    if t < 0x6C:
        return '%dL' % (t - 0x0C), p, 'var'
    if t < 0xCC:
        return '%dL[%dL,%d]' % (t - 0x6C, d[p] - 0x0C, d[p + 1]), p + 2, 'var'
    if t < 0xE5:
        return '$G%X' % (((t - 0xCC) << 8) | d[p]), p + 1, 'var'
    return '$G%X[%dL,%d]' % (((t - 0xE5) << 8) | d[p], d[p + 1] - 0x0C, d[p + 2]), p + 3, 'var'


def ins(d, p):
    """one instruction at p -> (opcode, name, [args], next p)"""
    raw = struct.unpack_from('<H', d, p)[0]
    op = raw & 0x7FFF
    c = CMDS.get(op)
    if c is None:
        raise Desync('unknown opcode %04X at %X' % (raw, p))
    q = p + 2
    args = []
    params = c.get('input', []) + c.get('output', [])
    n = c.get('num_params', len(params))
    if op in (0x05AE, 0x05AF):  # CALL: three raw bytes, the label, the in arguments
        nin = d[q] + d[q + 1]  # in and out arguments, no end byte
        args += [str(d[q]), str(d[q + 1]), str(d[q + 2])]
        q += 3
        a, q, _ = arg(d, q)
        args.append(a)
        for _ in range(nin):  # LCS: the in count, no end byte
            a, q, k = arg(d, q)
            args.append(a)
        return op, ('NOT ' if raw & 0x8000 else '') + c['name'], args, q
    types = [x.get('type') for x in params]
    for i in range(n):
        if i < len(types) and types[i] == 'arguments':
            while True:
                a, q, k = arg(d, q)
                if k == 'end':
                    break
                args.append(a)
        elif i < len(types) and types[i] in ('string', 'gxt_key', 'string8', 'zone_key') and d[q] > 0x0C and d[q] < 0x80 and chr(d[q]).isalnum():
            e = q + 8  # a raw 8-byte text (no type byte): LCS's short strings
            args.append("'%s'" % d[q:e].split(b'\0')[0].decode('latin1'))
            q = e
        else:
            a, q, k = arg(d, q)
            if k == 'end':
                raise Desync('type 0 in a fixed argument at %X (%s)' % (q - 1, c['name']))
            args.append(a)
    return op, ('NOT ' if raw & 0x8000 else '') + c['name'], args, q


def run(d, start, end, base, out):
    p = start
    count = 0
    while p < end:
        try:
            op, name, args, q = ins(d, p)
        except (Desync, IndexError, ValueError, struct.error) as e:
            out.write('; DESYNC %s\n' % e)
            return count, p
        out.write('%06X %04X %s %s\n' % (p - base, op, name, ' '.join(args)))
        count += 1
        p = q
    return count, None


if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    os.makedirs(sys.argv[2], exist_ok=True)
    main_size, largest = struct.unpack_from('<II', d, 0)
    print('main size %X, largest mission %X, file %X' % (main_size, largest, len(d)))
    count = struct.unpack_from('<I', d, 0x6476 + 0x10)[0]
    offs = [struct.unpack_from('<I', d, 0x6476 + 0x14 + 4 * i)[0] for i in range(count)]
    total = 0
    with open(os.path.join(sys.argv[2], 'main.txt'), 'w', encoding='utf-8') as out:
        n, bad = run(d, 0x6662, main_size + 8, 8, out)
    print('main: %d instructions%s' % (n, ', DESYNC at %X' % bad if bad is not None else ''))
    total += n
    bads = 0
    for i, o in enumerate(offs):
        end = offs[i + 1] if i + 1 < count else len(d) - 8
        with open(os.path.join(sys.argv[2], 'm%03d.txt' % i), 'w', encoding='utf-8') as out:
            n, bad = run(d, o + 8, end + 8, o + 8, out)
        total += n
        if bad is not None:
            bads += 1
            if bads <= 8:
                print('mission %d: DESYNC after %d at +%X' % (i, n, bad - o - 8))
    print('total %d instructions, %d missions desynced' % (total, bads))
