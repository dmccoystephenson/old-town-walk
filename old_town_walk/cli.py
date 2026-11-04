"""old-town-walk fetch | sanitize | verify | build"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .fetch import fetch, sha256, verify
from .geo import Area
from .osm import parse
from .sanitize import sanitize
from .tiles import Dem, build_all

ROOT = Path(__file__).resolve().parent.parent


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="old-town-walk")
    ap.add_argument("command", choices=["fetch", "sanitize", "verify", "build"])
    ap.add_argument("--area", type=Path, default=ROOT / "area.json")
    ap.add_argument("--raw", type=Path, default=ROOT / "data" / "raw")
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "tiles")
    ap.add_argument("--fetched", type=Path, default=ROOT / "build" / "fetched", help="full downloads, never committed")
    args = ap.parse_args(argv)
    area = Area.load(args.area)
    if args.command == "fetch":
        fetch(area, args.fetched)
        return 0
    if args.command == "sanitize":
        out = args.raw / "osm-sample.osm"
        print(sanitize(sorted(args.fetched.glob("osm-*.osm")), out))
        sources = json.loads((args.raw / "SOURCES.json").read_text())
        sources[out.name] = {"sha256": sha256(out), "bytes": out.stat().st_size}
        (args.raw / "SOURCES.json").write_text(json.dumps(sources, indent=1) + "\n")
        return 0
    if args.command == "verify":
        bad = verify(args.raw)
        print("raw data OK" if not bad else f"mismatch: {bad}")
        return 1 if bad else 0
    summary = build_all(area, parse(sorted(args.raw.glob("osm-*.osm"))), Dem(args.raw / "dem-3dep.tif"), args.out)
    print(f"wrote {args.out}: {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
