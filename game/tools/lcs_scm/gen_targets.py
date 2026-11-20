# The per-frame instructions of HANDOFF section 15 as C++ table rows for src/lcs/lcs_script_timing.cpp: each with
# the original bytes around it in MAIN.SCM (8 before, the instruction, 8 after), which the game's copy of the script
# (main or a loaded mission) holds unchanged.
# usage: python gen_targets.py <MAIN.SCM>
import os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lcsdis

d = open(sys.argv[1], 'rb').read()
count = struct.unpack_from('<I', d, 0x6476 + 0x10)[0]
offs = [struct.unpack_from('<I', d, 0x6476 + 0x14 + 4 * i)[0] for i in range(count)]
# (script, offset in its listing, why)
TARGETS = [
    ('m114', 0x00B5B, 'TOSH1 a beached boat loses 6 health a frame'),
    ('main', 0x31E0D, 'boat out of the water: frames counted to 30'),
    ('m086', 0x01BF4, 'SALS6 cruise speed -0.25 a frame'),
    ('m086', 0x01C15, 'SALS6 cruise speed +0.25 a frame'),
    ('m106', 0x02C51, 'SALH4 frames counted to 100'),
    ('m106', 0x02C6F, 'SALH4 cruise speed -0.5 a frame'),
    ('m106', 0x02C8C, 'SALH4 cruise speed +0.5 a frame'),
    ('m106', 0x02CB5, 'SALH4 37L -1 a frame'),
    ('m106', 0x02CD3, 'SALH4 37L +1 a frame'),
    ('m107', 0x047A8, 'SALH5 convoy speed -3 a frame'),
    ('m107', 0x047CE, 'SALH5 convoy speed -1 a frame'),
    ('m107', 0x047EE, 'SALH5 convoy speed +1 a frame'),
    ('m113', 0x01B4A, 'DONH6 slide speed +0.02 a frame'),
    ('m113', 0x01B6A, 'DONH6 slide speed +0.005 a frame'),
    ('m043', 0x01574, 'TS1 node search tries counted to 60'),
    ('m051', 0x01595, 'VIC5 Leone bleeds: frames counted to 5, then -1'),
    ('m057', 0x01BD8, 'SAL4 too slow: frames counted to 20'),
    ('m057', 0x01BF1, 'SAL4 scare: frames counted to 15, then +1'),
    ('m057', 0x00CBC, 'SAL4 frames counted to 300'),
    ('m057', 0x01D74, 'SAL4 upside down: frames counted to 55'),
    ('m107', 0x04822, 'SALH5 a stuck convoy car explodes after 500 frames'),
    # the user's report 2026-11-18: the "Sale" bar of the car / bike salesman jobs fills far too fast at high FPS.
    # Their score subroutines run every frame and add points while you drive fast / jump / do wheelies (and take
    # some off while you're too slow); one-off bonuses (jumps +300, a flag +250) and damage stay as they are.
    ('m034', 0x02067, 'CARS sale: +1 a frame (fast)'),
    ('m034', 0x020B4, 'CARS sale: +2 a frame (fast)'),
    ('m034', 0x020C7, 'CARS sale: +3 a frame (very fast)'),
    ('m034', 0x020E5, 'CARS sale: +5 a frame'),
    ('m034', 0x0212A, 'CARS sale: +2 a frame (slow driving)'),
    ('m034', 0x0213D, 'CARS sale: +2 a frame (very slow)'),
    ('m034', 0x021A3, 'CARS sale: +1 a frame (tilted)'),
    ('m034', 0x021B6, 'CARS sale: +2 a frame (tilted, fast)'),
    ('m034', 0x021C9, 'CARS sale: +1 a frame (tilted, very fast)'),
    ('m034', 0x021E7, 'CARS sale: +5 a frame (tilted)'),
    ('m034', 0x02268, 'CARS sale penalty: 1 a frame (too slow)'),
    ('m034', 0x02280, 'CARS sale penalty: 2 a frame (much too slow)'),
    ('m034', 0x0230C, 'CARS sale penalty: 3 a frame (too fast)'),
    ('m034', 0x02324, 'CARS sale penalty: 5 a frame (much too fast)'),
    ('m035', 0x01C1A, 'BIKE sale: +1 a frame (fast)'),
    ('m035', 0x01C67, 'BIKE sale: +1 a frame (fast)'),
    ('m035', 0x01C7A, 'BIKE sale: +1 a frame (very fast)'),
    ('m035', 0x01CF0, 'BIKE sale: +1 a frame (slow riding)'),
    ('m035', 0x01D03, 'BIKE sale: +1 a frame (very slow)'),
    ('m035', 0x01D3D, 'BIKE sale: +1 a frame (wheelie)'),
    ('m035', 0x01D50, 'BIKE sale: +1 a frame (wheelie, fast)'),
    ('m035', 0x01D6E, 'BIKE sale: +1 a frame'),
    ('m035', 0x01DF1, 'BIKE sale penalty: 1 a frame (too slow)'),
    ('m035', 0x01E09, 'BIKE sale penalty: 1 a frame (much too slow)'),
    ('m035', 0x01E96, 'BIKE sale penalty: 1 a frame (too fast)'),
    ('m035', 0x01EAE, 'BIKE sale penalty: 2 a frame (much too fast)'),
]
for key, off, why in TARGETS:
    base = 8 if key == 'main' else offs[int(key[1:])] + 8
    p = (off + 8) if key == 'main' else base + off
    op, name, args, q = lcsdis.ins(d, p)
    lo, hi = p - 8, q + 8
    window = d[lo:hi]
    print('        {0x%04X, %d, %d, "%s", "%s %s", {%s}}, // %s %s +0x%X' % (
        op, p - lo, q - p, why, name, ' '.join(args), ', '.join('0x%02X' % b for b in window), key, why, off))
