#!/usr/bin/env python3
"""Fresh ps2_recomp output -> generated/ee, as the game is built (docs/REPRODUCIBLE_BUILD_2026-11-17.md).

    python tools/lcs_postprocess.py <fresh recompiler output> <generated/ee>

1. Two codegen fixes the recompiler (lcs-recomp d8648c3) doesn't make itself:
   BGEZ / BLEZ / BLTZ / BGTZ compare the 64-bit register (GPR_S32 -> GPR_S64); SQRT.S takes ft (bits 16-20 of the
   instruction word, from the comment above it) instead of fs.
2. PS2_GUEST_CC on every function definition.
3. The RSL world hooks (tools/sync_generated.py).
4. The remaining edits (tools/lcs_codegen_edits.json): time-step patches, extra resume labels and registrations,
   diagnostics. Each original must match exactly once, else the run stops (the recompiler's output changed).
Only files whose bytes change are written (keeps ninja incremental); files gone from the output are removed.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sync_generated  # noqa: E402

BRANCH = re.compile(r'(const bool branch_taken_0x[0-9a-f]+ = \(GPR_)S32(\(ctx, \d+\) (?:>=|<=|<|>) 0\);)')
SQRT_MARK = re.compile(r'^(    ctx->f\[\d+\] = FPU_SQRT_S\(ctx->f\[\d+\]\);)$', re.M)
SQRT_FT = re.compile(r'(// 0x[0-9a-f]+: 0x([0-9a-f]+)[^\n]*\n(?:[^\n]*\n){0,2}?    ctx->f\[\d+\] = FPU_SQRT_S\(ctx->f\[)\d+(\]\);)')
GUEST_CC = re.compile(r'^void ((?:sub|entry)_[0-9A-Fa-f_x]+\(uint8_t\*)', re.M)


def codegen_fixes(src):
    src = BRANCH.sub(r'\1S64\2', src)
    src = SQRT_MARK.sub(r'\1 /* [lcs] SQRT.S reads ft */', src)
    src = SQRT_FT.sub(lambda m: m.group(1) + str((int(m.group(2), 16) >> 16) & 31) + m.group(3), src)
    return GUEST_CC.sub(r'PS2_GUEST_CC void \1', src)


def load_edits():
    with open(os.path.join(HERE, 'lcs_codegen_edits.json'), encoding='utf-8') as f:
        edits = json.load(f)['edits']
    by_file = {}
    for e in edits:
        by_file.setdefault(e['file'], []).append(e)
    return by_file


def process(name, data, edits):
    src = codegen_fixes(data.decode('utf-8', 'surrogateescape').replace('\r\n', '\n'))
    src = sync_generated.wire_lcs_submission_stores(name, src.encode('utf-8', 'surrogateescape')).decode(
        'utf-8', 'surrogateescape')
    for e in edits.get(name, []):
        if src.count(e['original']) != 1:
            raise SystemExit(f"{name}: an edit's original isn't there exactly once (the recompiler output changed)")
        src = src.replace(e['original'], e['replacement'])
    return src.encode('utf-8', 'surrogateescape')


def main(fresh, dest):
    edits = load_edits()
    os.makedirs(dest, exist_ok=True)
    names = set(os.listdir(fresh))
    missing = set(edits) - names
    if missing:
        raise SystemExit(f'edits for files the recompiler did not write: {sorted(missing)}')
    changed = 0
    for name in sorted(names):
        with open(os.path.join(fresh, name), 'rb') as f:
            out = process(name, f.read(), edits)
        d = os.path.join(dest, name)
        if os.path.exists(d):
            with open(d, 'rb') as f:
                if f.read() == out:
                    continue
        with open(d, 'wb') as f:
            f.write(out)
        changed += 1
    removed = 0
    for name in os.listdir(dest):
        if name not in names:
            os.remove(os.path.join(dest, name))
            removed += 1
    print(f'generated: {changed} written, {removed} removed, {len(names)} total')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
