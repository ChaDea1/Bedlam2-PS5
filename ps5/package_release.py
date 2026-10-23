#!/usr/bin/env python3
"""Assemble a ProsperoStore-compatible PS5 app ZIP without game data."""

from __future__ import annotations

import argparse
import json
import stat
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TITLE_ID = "PPSA94003"
APP_METADATA = ROOT / TITLE_ID


def add_file(archive: zipfile.ZipFile, source: Path, archive_name: str) -> None:
    info = zipfile.ZipInfo(archive_name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (stat.S_IFREG | 0o644) << 16
    archive.writestr(info, source.read_bytes())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--eboot", type=Path, required=True, help="built native eboot.bin")
    parser.add_argument("--libc-prx", type=Path, required=True, help="local PS5 runtime libc.prx")
    parser.add_argument("--output", type=Path, required=True, help="destination ZIP path")
    args = parser.parse_args()

    param_path = APP_METADATA / "sce_sys" / "param.json"
    icon_path = APP_METADATA / "sce_sys" / "icon0.png"
    for path in (args.eboot, args.libc_prx, param_path, icon_path):
        if not path.is_file():
            parser.error(f"required file does not exist: {path}")

    metadata = json.loads(param_path.read_text(encoding="utf-8"))
    if metadata.get("titleId") != TITLE_ID:
        parser.error(f"{param_path} must declare titleId {TITLE_ID}")
    if not metadata.get("contentVersion"):
        parser.error(f"{param_path} must declare contentVersion")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w") as archive:
        add_file(archive, args.eboot, f"{TITLE_ID}/eboot.bin")
        add_file(archive, args.libc_prx, f"{TITLE_ID}/sce_module/libc.prx")
        add_file(archive, param_path, f"{TITLE_ID}/sce_sys/param.json")
        add_file(archive, icon_path, f"{TITLE_ID}/sce_sys/icon0.png")

    with zipfile.ZipFile(args.output) as archive:
        names = set(archive.namelist())
        if any("assets/bed2/" in name.lower() for name in names):
            args.output.unlink(missing_ok=True)
            parser.error("refusing to package original Bedlam 2 game data")
        expected = {
            f"{TITLE_ID}/eboot.bin",
            f"{TITLE_ID}/sce_module/libc.prx",
            f"{TITLE_ID}/sce_sys/param.json",
            f"{TITLE_ID}/sce_sys/icon0.png",
        }
        if names != expected:
            args.output.unlink(missing_ok=True)
            parser.error("package contents did not match the expected app-only file list")

    print(f"Created {args.output} with app binaries and metadata only; no game data included.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
