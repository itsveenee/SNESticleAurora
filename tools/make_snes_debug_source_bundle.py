#!/usr/bin/env python3
"""Build a deterministic tar.gz of the exact SNES/Aurora source used by a debug build."""
from pathlib import Path
import gzip
import hashlib
import io
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/embed/snes_debug_sources.tar.gz"
if not OUT.is_absolute():
    OUT = ROOT / OUT

EXTS = {".c", ".cc", ".cpp", ".h", ".hpp", ".s", ".S", ".inc", ".inl"}
files = set()

snes = ROOT / "src/snes"
for p in snes.rglob("*"):
    if p.is_file() and p.suffix in EXTS and ".pre_" not in p.name and not p.name.endswith(".orig"):
        files.add(p)

for base in [ROOT / "src/platform/ps2/system", ROOT / "src/platform/ps2/ui"]:
    if base.is_dir():
        for p in base.iterdir():
            if not p.is_file() or p.suffix not in EXTS or ".pre_" in p.name:
                continue
            if p.name.startswith("mainloop") or p.name in {
                "aurora_runtime_trace.c", "aurora_runtime_trace.h",
                "aurora_snes_cost_profiler.cpp", "aurora_snes_cost_profiler.h",
                "uiVideo.cpp", "uiVideo.h",
            }:
                files.add(p)

for rel in [
    "Makefile",
    "src/app/emusys.cpp", "src/app/emusys.h",
    "src/app/emurom.cpp", "src/app/emurom.h",
    "src/common/base/types.h",
    "src/common/render/mixbuffer.cpp", "src/common/render/mixbuffer.h",
    "src/common/render/rendersurface.cpp", "src/common/render/rendersurface.h",
    "tools/make_snes_debug_source_bundle.py",
    "tools/extract_snes_debug_state.py",
]:
    p = ROOT / rel
    if p.is_file():
        files.add(p)

files = sorted(files, key=lambda p: p.relative_to(ROOT).as_posix())

try:
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
except Exception:
    head = "unknown"
try:
    status = subprocess.check_output(["git", "status", "--short", "--untracked-files=no"], cwd=ROOT, text=True)
except Exception:
    status = "unavailable\n"

manifest = [
    "SNESticleAurora SNES debug source bundle v1",
    f"git_head={head}",
    "",
    "git_status:",
    status.rstrip("\n"),
    "",
    "sha256:",
]
for p in files:
    data = p.read_bytes()
    manifest.append(f"{hashlib.sha256(data).hexdigest()}  {p.relative_to(ROOT).as_posix()}")
manifest_bytes = ("\n".join(manifest) + "\n").encode("utf-8")

OUT.parent.mkdir(parents=True, exist_ok=True)
with OUT.open("wb") as raw:
    with gzip.GzipFile(filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=0) as gz:
        with tarfile.open(fileobj=gz, mode="w", format=tarfile.GNU_FORMAT) as tf:
            def add_bytes(name: str, data: bytes, mode: int = 0o644):
                ti = tarfile.TarInfo(name)
                ti.size = len(data)
                ti.mtime = 0
                ti.mode = mode
                ti.uid = ti.gid = 0
                ti.uname = ti.gname = ""
                tf.addfile(ti, io.BytesIO(data))

            add_bytes("BUILD_MANIFEST.txt", manifest_bytes)
            for p in files:
                rel = p.relative_to(ROOT).as_posix()
                add_bytes(rel, p.read_bytes(), 0o755 if p.suffix == ".py" else 0o644)

print(f"{OUT} ({OUT.stat().st_size} bytes, {len(files)} source files)")
