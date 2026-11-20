#!/usr/bin/env python3
"""The build kit the launcher ships: everything to build LCSrecomp.exe on the player's PC from their ISO - and no game
data or game code (generated/, build/, ISOs, ELFs, memory cards are never copied; checked at the end).

    python tools/make_kit.py <out dir> <lcs-recomp checkout> <guest.toml> <vu1_uploads.txt>

<out>/source      the runtime's sources (this repository's CMakeLists.txt, src/, third_party/PS2Recomp without its
                  recompiler, the build's tools/, deps/)
<out>/recompiler  ps2xRecomp at lcs-recomp d8648c3 (the codegen generated/ee is made with, see
                  docs/REPRODUCIBLE_BUILD_2026-11-17.md), its CMake and the runtime headers it compiles in
<out>/config      lcs_guest.toml (input / output filled in by the launcher), vu1_uploads.txt, tools.json (the build
                  tools the launcher downloads: URL, SHA-256, size - pinned here so a kit update can change them)
<out>/kit.json    version, the recompiler's commit, every file with its size and SHA-256 (the launcher syncs its
                  build tree by this list; a release's signed manifest carries it)
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import io
import hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RECOMPILER_COMMIT = "d8648c3"
BUILD_TOOLS = ["hlsl_dxbc.py", "hlsl_spirv.py", "lcs_postprocess.py", "lcs_codegen_edits.json", "sync_generated.py",
               "vu1_recomp.py"]
NEVER = re.compile(r"(\.iso|\.elf|SLUS_\d+\.\d+|\.ps2|\.bin|\.pdb|\.obj|\.o)$", re.I)
# the build tools the launcher downloads (official downloads, hashed 2026-11-17; the launcher refuses anything but
# https and checks size and SHA-256). "folder": the folder under toolchain\ it ends up in; "inner": the zip has it at
# its top. Order matters: llvm-mingw, CMake, Ninja, Python.
TOOLS = [
    {"name": "llvm-mingw 20261020",
     "url": "https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip",
     "sha256": "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd", "size": 190612575,
     "folder": "llvm-mingw-20261020-ucrt-x86_64", "inner": True},
    {"name": "CMake 4.4.5",
     "url": "https://github.com/Kitware/CMake/releases/download/v4.4.5/cmake-4.4.5-windows-x86_64.zip",
     "sha256": "326c826486d46a23cf9d5a1c9ee3a567d427bdd02c4a34071937a2a675cff004", "size": 54412232,
     "folder": "cmake-4.4.5-windows-x86_64", "inner": True},
    {"name": "Ninja 1.13.2", "url": "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
     "sha256": "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65", "size": 291570,
     "folder": "ninja-1.13.2", "inner": False},
    {"name": "Python 3.14.7 (embeddable)",
     "url": "https://www.python.org/ftp/python/3.14.7/python-3.14.7-embed-amd64.zip",
     "sha256": "d297e5ff019966817ad8502465176139f2d3d840fa4ed84b13bed399a6ab1f15", "size": 12673227,
     "folder": "python-3.14.7", "inner": False},
]

# PS2Recomp's parts the runtime doesn't build; libraries in deps/ are copied whole (their own "generated" headers etc.)
SKIP_DIRS = {"build", "out", ".git", "__pycache__", "ps2xTest", "ps2xStudio", "ps2xAnalyzer", "android", "vita",
             "ps2xRecomp"}

# the same neutral wording as the sources (VCS mentions in the recompiler's comments)
SUBS = [(rb"Vice City Stories", rb"the reference game"), (rb"Vice City", rb"the reference game"),
        (rb"\bvcs\.exe\b", rb"LCSrecomp.exe"), (rb"\bvcs\.ini\b", rb"lcs.ini"), (rb"\bvcs-recomp\b", rb"lcs-recomp"),
        (rb"\bgta-recomp\b", rb"lcs-recomp"), (rb"\bgame/vcs\b", rb"game/lcs"),
        (rb"\bvcs_([a-z0-9_]+)\.(cpp|h|py)\b", rb"ref_\1.\2"), (rb"\bVCS_([A-Z0-9_]+)\b", rb"LCS_\1"),
        (rb"\bVCS's\b", rb"the reference port's"), (rb"\bVCS\b", rb"the reference port"), (rb"\bVcs([A-Z])", rb"Lcs\1"),
        (rb"Vcs\b", rb"Lcs"), (rb"\bvcs\b", rb"lcs")]


def copy_tree(src, dst, skip=SKIP_DIRS):
    for dp, dn, fn in os.walk(src):
        dn[:] = [d for d in dn if d not in skip]
        rel = os.path.relpath(dp, src)
        for f in fn:
            if NEVER.search(f):
                continue
            os.makedirs(os.path.join(dst, rel), exist_ok=True)
            shutil.copy2(os.path.join(dp, f), os.path.join(dst, rel, f))


def neutral(path):
    for dp, _, fn in os.walk(path):
        for f in fn:
            if f.startswith(("LICENSE", "COPYING")) or not f.endswith((".cpp", ".h", ".hpp", ".txt", ".cmake", ".md")):
                continue
            p = os.path.join(dp, f)
            b = open(p, "rb").read()
            n = b
            for a, z in SUBS:
                n = re.sub(a, z, n)
            if n != b:
                open(p, "wb").write(n)


def main(out, lcs_recomp, guest, uploads):
    if os.path.exists(out):
        shutil.rmtree(out)
    src = os.path.join(out, "source")
    os.makedirs(src)
    shutil.copy2(os.path.join(ROOT, "CMakeLists.txt"), src)
    copy_tree(os.path.join(ROOT, "src"), os.path.join(src, "src"), skip={"__pycache__"})
    copy_tree(os.path.join(ROOT, "third_party", "PS2Recomp"), os.path.join(src, "third_party", "PS2Recomp"))
    copy_tree(os.path.join(ROOT, "deps"), os.path.join(src, "deps"), skip={"__pycache__", ".git"})
    os.makedirs(os.path.join(src, "tools"))
    for t in BUILD_TOOLS:
        shutil.copy2(os.path.join(HERE, t), os.path.join(src, "tools", t))

    # the recompiler at the commit the game's code is made with (git archive: nothing in the checkout changes)
    rec = os.path.join(out, "recompiler")
    data = subprocess.run(["git", "-C", lcs_recomp, "archive", RECOMPILER_COMMIT, "third_party/PS2Recomp"],
                          check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        tar.extractall(os.path.join(out, "_rec"), filter="data")
    base = os.path.join(out, "_rec", "third_party", "PS2Recomp")
    os.makedirs(rec)
    for f in ("CMakeLists.txt", "LICENSE", "README.md"):
        if os.path.exists(os.path.join(base, f)):
            shutil.copy2(os.path.join(base, f), rec)
    copy_tree(os.path.join(base, "ps2xRecomp"), os.path.join(rec, "ps2xRecomp"), skip={"build", "out", ".git"})
    copy_tree(os.path.join(base, "ps2xRuntime", "include"), os.path.join(rec, "ps2xRuntime", "include"))
    copy_tree(os.path.join(base, "ps2xRuntime", "cmake"), os.path.join(rec, "ps2xRuntime", "cmake"))
    shutil.rmtree(os.path.join(out, "_rec"))
    neutral(rec)

    cfg = os.path.join(out, "config")
    os.makedirs(cfg)
    text = open(guest, encoding="utf-8").read()
    text = re.sub(r"(?m)^input = .*$", 'input = "@ELF@"', text)
    text = re.sub(r"(?m)^output = .*$", 'output = "@OUT@"', text)
    text = re.sub(r"(?m)^# PS2Recomp configuration for: .*$", "# LCS Recomp: ps2xRecomp configuration for SLUS_214.23", text)
    open(os.path.join(cfg, "lcs_guest.toml"), "w", encoding="utf-8", newline="\n").write(text)
    shutil.copy2(uploads, os.path.join(cfg, "vu1_uploads.txt"))
    with open(os.path.join(cfg, "tools.json"), "w", encoding="utf-8", newline="\n") as h:
        json.dump({"tools": TOOLS}, h, indent=1)

    # nothing game-derived may be in the kit
    count = 0
    for dp, dn, fn in os.walk(out):
        for f in fn:
            count += 1
            p = os.path.join(dp, f)
            if NEVER.search(f):
                sys.exit(f"game data in the kit: {p}")
            with open(p, "rb") as h:
                head = h.read(4)
            if head == b"\x7fELF" and "deps" not in p:
                sys.exit(f"an ELF in the kit: {p}")
    # the licences of everything in the kit, one file each (shipped with the launcher, shown in About)
    lic = os.path.join(out, "licenses")
    os.makedirs(lic)
    for name, base in [("PS2Recomp", os.path.join(src, "third_party", "PS2Recomp"))] +             [(d, os.path.join(src, "deps", d)) for d in sorted(os.listdir(os.path.join(src, "deps")))
             if os.path.isdir(os.path.join(src, "deps", d))]:
        for f in sorted(os.listdir(base)):
            if f.upper().startswith(("LICENSE", "COPYING", "LICENCE")) and os.path.isfile(os.path.join(base, f)):
                shutil.copy2(os.path.join(base, f), os.path.join(lic, f"{name}-{f}.txt".replace(".txt.txt", ".txt")))
    shutil.copy2(os.path.join(ROOT, "NOTICE.txt"), os.path.join(out, "NOTICE.txt"))  # origin and licences
    shutil.copy2(os.path.join(ROOT, "LICENSE.txt"), os.path.join(out, "LICENSE.txt"))  # LCS Recomp: GPL-3.0
    version = open(os.path.join(ROOT, "VERSION"), encoding="utf-8").read().strip()
    files = []
    for dp, dn, fn in os.walk(out):
        dn.sort()
        for f in sorted(fn):
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, out).replace(os.sep, "/")
            if rel == "kit.json":
                continue
            data = open(p, "rb").read()
            files.append({"path": rel, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    with open(os.path.join(out, "kit.json"), "w", encoding="utf-8", newline="\n") as h:
        json.dump({"kit": 2, "version": version, "recompiler_commit": RECOMPILER_COMMIT, "file_count": len(files),
                   "files": files}, h, indent=1)
    print(f"kit {version}: {len(files)} files in {out}")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
