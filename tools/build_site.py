"""Assembles the site the browser plays, and refuses one that names the machine it was built on.

  python tools/build_site.py <build-web> <tiles> <out>

The tiles go up gzipped, at level 9, and the page unpacks them, so they stay small on any host and
nothing has to compress them on the way. It uses only the standard library.
"""

from __future__ import annotations

import gzip
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LEAKS = re.compile(rb"/home/(?!web_user)[a-z]|/Users/[A-Za-z]|/root/|/mnt/c|[A-Z]:\\\\?Users|gmail\.com")


def gzipped(data: bytes) -> bytes:
    return gzip.compress(data, compresslevel=9, mtime=0)


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    build, tiles, out = Path(argv[1]), Path(argv[2]), Path(argv[3])
    kit = build / "_deps" / "strata-src" / "web" / "kit"
    if out.exists():
        shutil.rmtree(out)
    (out / "tiles").mkdir(parents=True)
    shutil.copy(ROOT / "web" / "index.html", out / "index.html")
    wasm = (build / "walk.wasm").read_bytes()
    page = (ROOT / "web" / "page.js").read_text().replace("__WASM_BYTES__", str(len(wasm)))
    (out / "page.js").write_text(page)
    for name in ("walk.js", "walk.wasm"):
        shutil.copy(build / name, out / name)
    for name in ("strata-kit.js", "strata-loader.js"):
        shutil.copy(kit / name, out / name)
    shutil.copy(ROOT / "docs" / "images" / "street.png", out / "poster.png")
    shutil.copy(tiles / "manifest.txt", out / "tiles" / "manifest.txt")
    raw = packed = 0
    for tile in sorted(tiles.glob("*.tile")):
        data = tile.read_bytes()
        small = gzipped(data)
        (out / "tiles" / (tile.name + ".gz")).write_bytes(small)
        raw += len(data)
        packed += len(small)

    bad = []
    for path in sorted(out.rglob("*")):
        if path.is_file():
            if path.suffix == ".map":
                bad.append(f"{path.name}: a source map")
            elif path.suffix in (".wasm", ".js", ".html", ".txt") and LEAKS.search(path.read_bytes()):
                bad.append(f"{path.name}: {LEAKS.search(path.read_bytes()).group()!r}")
    if bad:
        print("site refused:", *bad, sep="\n  ", file=sys.stderr)
        return 1
    engine = len(gzipped(wasm)) + len(gzipped((out / "walk.js").read_bytes()))
    shell = sum(len(gzipped((out / n).read_bytes())) for n in ("index.html", "page.js", "strata-kit.js", "strata-loader.js"))
    print(f"tiles: {raw} bytes, {packed} gzipped")
    print(f"engine: {len(wasm)} byte wasm, {engine} gzipped with its glue; page and kit {shell} gzipped")
    print(f"site: {sum(p.stat().st_size for p in out.rglob('*') if p.is_file())} bytes in {sum(1 for p in out.rglob('*') if p.is_file())} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
