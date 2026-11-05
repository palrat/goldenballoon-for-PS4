#!/usr/bin/env python3
"""make_pkg.py — package the PS4 DKR build into an installable .pkg.

Steps: patch a param.sfo template to this app's identity, write the .gp4 file
list, and run the toolchain's PkgTool.Core. The SFO is edited in place: the
format is a header plus two tables (keys, values), so a value that fits its
declared maximum is overwritten with zero padding and everything else stays.

Usage: make_pkg.py --template <sce_sys dir> --eboot <eboot.bin> --out <dir>
"""
import argparse
import shutil
import struct
import os
import subprocess
import sys
from pathlib import Path

TITLE = "Diddy Kong Racing"
TITLE_ID = "CUSA64004"
CONTENT_ID = "UP0001-CUSA64004_00-0000000000000001"

# The native Linux PkgTool.Core links against a libssl the Arch system no
# longer provides ("No usable version of libssl was found") and dies with
# SIGABRT before writing anything. The Windows build of the same toolchain
# runs fine under the .NET runtime, so that is what gets invoked here.
# Follows the same toolchain the build uses (OO_PS4_TOOLCHAIN).
if not os.environ.get("OO_PS4_TOOLCHAIN"):
    sys.exit("make_pkg.py: set OO_PS4_TOOLCHAIN to your OpenOrbis PS4Toolchain directory")
PKGTOOL_DLL = os.path.join(os.environ["OO_PS4_TOOLCHAIN"], "bin", "windows", "PkgTool.Core.dll")
DOTNET = shutil.which("dotnet") or os.path.expanduser("~/.dotnet/dotnet")
PKGTOOL = [DOTNET, PKGTOOL_DLL]


def read_sfo(path: Path):
    data = path.read_bytes()
    magic, version, key_off, data_off, count = struct.unpack_from("<4sIIII", data, 0)
    if magic != b"\0PSF":
        raise SystemExit(f"not an SFO: {magic!r}")
    entries = []
    for i in range(count):
        koff, fmt, dlen, dmax, doff = struct.unpack_from("<HHIII", data, 20 + i * 16)
        name = data[key_off + koff:data.find(b"\0", key_off + koff)].decode()
        entries.append((name, fmt, dlen, dmax, doff))
    return data, entries


def set_value(data: bytearray, entries, name: str, value: str) -> None:
    for (n, fmt, dlen, dmax, doff) in entries:
        if n != name:
            continue
        raw = value.encode() + b"\0"
        if len(raw) > dmax:
            raise SystemExit(f"{name}: '{value}' needs {len(raw)} bytes, max {dmax}")
        data[data_off_base + doff:data_off_base + doff + dmax] = raw.ljust(dmax, b"\0")
        # length field lives at 20 + index*16 + 4
        return
    raise SystemExit(f"{name} not present in the SFO template")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--template", required=True)
    ap.add_argument("--eboot", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--title", default=TITLE)
    ap.add_argument("--title-id", default=TITLE_ID)
    ap.add_argument("--content-id", default=CONTENT_ID)
    ap.add_argument("--app-ver", default="01.02")
    args = ap.parse_args()

    template = Path(args.template)
    out = Path(args.out)
    eboot = Path(args.eboot)
    sce_sys = out / "sce_sys"
    if sce_sys.exists():
        shutil.rmtree(sce_sys)
    shutil.copytree(template, sce_sys)

    sfo = sce_sys / "param.sfo"
    data, entries = read_sfo(sfo)
    header = struct.unpack_from("<4sIIII", data, 0)
    global data_off_base
    data_off_base = header[3]

    data = bytearray(data)
    for name, value in (("TITLE", args.title),
                        ("TITLE_ID", args.title_id),
                        ("CONTENT_ID", args.content_id),
                        ("APP_VER", args.app_ver)):
        set_value(data, entries, name, value)

    # Length fields must match the new strings.
    for i, (name, fmt, dlen, dmax, doff) in enumerate(entries):
        if name in ("TITLE", "TITLE_ID", "CONTENT_ID", "APP_VER"):
            new = {"TITLE": args.title, "TITLE_ID": args.title_id,
                   "CONTENT_ID": args.content_id,
                   "APP_VER": args.app_ver}[name].encode() + b"\0"
            struct.pack_into("<I", data, 20 + i * 16 + 4, len(new))
    sfo.write_bytes(data)

    shutil.copy2(args.eboot, out / "eboot.bin")

    files = ['    <file targ_path="sce_sys/param.sfo" orig_path="sce_sys/param.sfo"/>']
    for p in sorted(sce_sys.rglob("*")):
        if p.is_dir() or p.name == "param.sfo":
            continue
        rel = p.relative_to(out).as_posix()
        files.append(f'    <file targ_path="{rel}" orig_path="{rel}"/>')
    files.append('    <file targ_path="eboot.bin" orig_path="eboot.bin"/>')

    # The gp4 declares its directory tree separately from the file list; without
    # <rootdir> LibOrbisPkg fails in FindDir before it reads a single file.
    (out / "sce_module").mkdir(exist_ok=True)
    gp4 = out / "dkr-ps4.gp4"
    gp4.write_text(f"""<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<psproject fmt="gp4" version="1000">
\t<volume>
\t\t<volume_type>pkg_ps4_app</volume_type>
\t\t<volume_id>PS4VOLUME</volume_id>
\t\t<volume_ts>2026-11-02 12:00:00</volume_ts>
\t\t<package content_id="{args.content_id}" passcode="00000000000000000000000000000000" c_date="2026-11-02" storage_type="bd25" app_type="full"/>
\t\t<chunk_info chunk_count="1" scenario_count="1">
\t\t\t<chunks>
\t\t\t\t<chunk id="0" layer_no="0" label="Chunk #0"/>
\t\t\t</chunks>
\t\t\t<scenarios default_id="0">
\t\t\t\t<scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0">0</scenario>
\t\t\t</scenarios>
\t\t</chunk_info>
\t</volume>
\t<files img_no="0">
{chr(10).join(files)}
\t</files>
\t<rootdir>
\t\t<dir targ_name="sce_sys">
\t\t\t<dir targ_name="about"/>
\t\t</dir>
\t\t<dir targ_name="sce_module"/>
\t</rootdir>
</psproject>
""")

    print(f"[pkg] {gp4}")
    # PkgTool.Core aborts with SIGABRT (exit 134, "Unhandled exception:
    # DirectoryNotFoundException") when the output directory does not exist,
    # and it writes nothing in that case. Piping its output through tail can
    # hide that, so the directory is created here and the result is checked
    # against the eboot's timestamp: a package older than the eboot means the
    # tool aborted and we would otherwise ship the previous build.
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["DOTNET_ROLL_FORWARD"] = "LatestMajor"
    env["DOTNET_SYSTEM_GLOBALIZATION_INVARIANT"] = "1"
    rc = subprocess.run(PKGTOOL + ["pkg_build", str(gp4), str(out)],
                        check=False, env=env)
    if rc.returncode != 0:
        sys.exit(f"[pkg] PkgTool.Core failed with exit {rc.returncode}")

    produced = sorted(out.glob("*.pkg"), key=lambda f: f.stat().st_mtime)
    if not produced:
        sys.exit("[pkg] PkgTool.Core reported success but wrote no package")
    newest = produced[-1]
    if eboot.exists() and newest.stat().st_mtime + 1 < eboot.stat().st_mtime:
        sys.exit(f"[pkg] {newest.name} is older than the eboot — packaging "
                 f"did not pick up the current build")
    print(f"[pkg] ok: {newest.name} ({newest.stat().st_size} bytes)")
    sys.exit(0)
