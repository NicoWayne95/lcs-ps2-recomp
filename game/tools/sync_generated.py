#!/usr/bin/env python3
"""Copy freshly generated C++ into generated/ee, touching only changed files.

ps2_recomp rewrites every file on each run, which would make ninja rebuild
all ~300 unity batches (~20 min). Syncing by content keeps rebuilds incremental.

    python tools/sync_generated.py <fresh dir> <dest dir>
"""
import os
import shutil
import sys


# LCS US SLUS_214.23, at the final REF/CALL stores after the state flush.
LCS_DRAW_STORES = {
    "sub_0029FAB8_0x29fab8.cpp": ("29fb9c", "29fbb0", 2, 8, "false"),
    "sub_0029FBC0_0x29fbc0.cpp": ("29fcc4", "29fcd8", 4, 16, "false"),
    "sub_0029FD40_0x29fd40.cpp": ("29fe74", "29fe88", 2, 8, "true"),
}


LCS_GS_QUEUE_STORES = {
    "sub_0029F708_0x29f708.cpp": "29f85c",
    "sub_0029F8E0_0x29f8e0.cpp": "29fa34",
    "sub_0029FAB8_0x29fab8.cpp": "29fb10",
    "sub_0029FBC0_0x29fbc0.cpp": "29fc20",
    "sub_0029FD40_0x29fd40.cpp": "29fd98",
}

LCS_UPLOAD = "sub_00321CB0_0x321cb0.cpp"
LCS_SUBMISSION_INCLUDE = (
    '#include "ps2_runtime.h"',
    '#include "ps2_runtime.h"\n#include "game/lcs_rsl_world.h"',
)


def upload_store_edits():
    edits = [
        ("    switch (ctx->pc) {", "    bool submittedUpload = false;\n    switch (ctx->pc) {"),
        (
            "    ctx->pc = 0x321f98u;\n"
            "    WRITE64(ADD32(GPR_U32(ctx, 12), 16), GPR_U64(ctx, 2));",
            "    ctx->pc = 0x321f98u;\n"
            "    submittedUpload = lcs::rslworld::submitLcsUpload(rdram, GPR_U32(ctx, 12), GPR_U64(ctx, 5), GPR_U64(ctx, 2));\n"
            "    if (!submittedUpload)\n"
            "        WRITE64(ADD32(GPR_U32(ctx, 12), 16), GPR_U64(ctx, 2));",
        ),
    ]
    for pc, write in (
        ("321fa0", "WRITE64(ADD32(GPR_U32(ctx, 12), 0), GPR_U64(ctx, 5));"),
        ("321fa4", "WRITE32(ADD32(GPR_U32(ctx, 12), 8), GPR_U32(ctx, 7));"),
        ("321fa8", "WRITE32(ADD32(GPR_U32(ctx, 12), 12), GPR_U32(ctx, 4));"),
        ("321fcc", "WRITE32(ADD32(GPR_U32(ctx, 14), 8), GPR_U32(ctx, 0));"),
    ):
        prefix = f"    ctx->pc = 0x{pc}u;\n"
        edits.append((prefix + "    " + write, prefix + "    if (!submittedUpload)\n        " + write))
    pixel_command = "WRITE32(ADD32(GPR_U32(ctx, 14), 12), GPR_U32(ctx, 6));"
    edits.append(("        " + pixel_command, "        if (!submittedUpload)\n            " + pixel_command))
    return edits


def lcs_submission_edits(name):
    if name != LCS_UPLOAD and name not in LCS_DRAW_STORES and name not in LCS_GS_QUEUE_STORES:
        return []
    if name == LCS_UPLOAD:
        edits = upload_store_edits()
    elif name in LCS_DRAW_STORES:
        low_pc, high_pc, reg, stack, model_b = LCS_DRAW_STORES[name]
        edits = [
            ("    switch (ctx->pc) {", "    bool submittedDraw = false;\n    switch (ctx->pc) {"),
            (
                f"    ctx->pc = 0x{low_pc}u;\n    WRITE64(ADD32(GPR_U32(ctx, 3), 0), GPR_U64(ctx, {reg}));",
                f"    ctx->pc = 0x{low_pc}u;\n"
                f"    submittedDraw = lcs::rslworld::submitLcsDraw(rdram, GPR_U32(ctx, 3), GPR_U64(ctx, {reg}),\n"
                f"        READ32(ADD32(GPR_U32(ctx, 29), {stack})), {model_b});\n"
                f"    if (!submittedDraw)\n        WRITE64(ADD32(GPR_U32(ctx, 3), 0), GPR_U64(ctx, {reg}));",
            ),
            (
                f"    ctx->pc = 0x{high_pc}u;\n    WRITE64(ADD32(GPR_U32(ctx, 3), 0), GPR_U64(ctx, 0));",
                f"    ctx->pc = 0x{high_pc}u;\n    if (!submittedDraw)\n"
                "        WRITE64(ADD32(GPR_U32(ctx, 3), 0), GPR_U64(ctx, 0));",
            ),
        ]
    else:
        edits = []
    if name in LCS_GS_QUEUE_STORES:
        prefix = f"    ctx->pc = 0x{LCS_GS_QUEUE_STORES[name]}u;\n"
        store = "WRITE64(ADD32(GPR_U32(ctx, 8), 0), GPR_U64(ctx, 3));"
        edits.append((
            prefix + "    " + store,
            prefix + "    if (!lcs::rslworld::submitLcsState(rdram, GPR_U32(ctx, 8), GPR_U64(ctx, 3)))\n        " + store,
        ))
    return [LCS_SUBMISSION_INCLUDE, *edits]


def wire_lcs_submission_stores(name, data):
    edits = lcs_submission_edits(name)
    if not edits:
        return data
    nl = b"\r\n" if b"\r\n" in data else b"\n"
    src = data.decode("utf-8").replace("\r\n", "\n")
    for original, replacement in edits:
        if src.count(original) != 1:
            raise ValueError(f"{name}: the generated LCS submission store changed: {original!r}")
        src = src.replace(original, replacement)
    return src.encode("utf-8").replace(b"\n", nl)


def main(src, dst):
    os.makedirs(dst, exist_ok=True)
    fresh = set(os.listdir(src))
    changed = added = removed = 0
    for name in fresh:
        s = os.path.join(src, name)
        d = os.path.join(dst, name)
        with open(s, "rb") as f:
            data = f.read()
        data = wire_lcs_submission_stores(name, data)
        if os.path.exists(d):
            with open(d, "rb") as f:
                if f.read() == data:
                    continue
            changed += 1
        else:
            added += 1
        with open(d, "wb") as f:
            f.write(data)
    for name in os.listdir(dst):
        if name not in fresh:
            os.remove(os.path.join(dst, name))
            removed += 1
    shutil.rmtree(src)
    print("generated: %d changed, %d added, %d removed, %d total" % (changed, added, removed, len(fresh)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
