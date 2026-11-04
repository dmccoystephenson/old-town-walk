"""Assembles the download: python tools/package_zip.py path/to/old-town-walk.exe build/tiles out.zip"""

from __future__ import annotations

import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOP = "old-town-walk"
STAMP = (2026, 1, 1, 0, 0, 0)   # fixed, so a zip says nothing about when or where it was built


def add(z: zipfile.ZipFile, source: Path, name: str, executable: bool = False) -> None:
    info = zipfile.ZipInfo(f"{TOP}/{name}", STAMP)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (0o755 if executable else 0o644) << 16
    z.writestr(info, source.read_bytes())


def main() -> int:
    exe, tiles, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    with zipfile.ZipFile(out, "w") as z:
        add(z, exe, "old-town-walk.exe", executable=True)
        add(z, ROOT / "docs" / "README-download.txt", "README.txt")
        add(z, ROOT / "LICENSE", "LICENSE.txt")
        add(z, ROOT / "NOTICE.md", "NOTICE.txt")
        for path in sorted(tiles.iterdir()):
            add(z, path, f"tiles/{path.name}")
    print(f"{out}: {out.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
