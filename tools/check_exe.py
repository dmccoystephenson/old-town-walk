"""Fails if a Windows build needs a Visual C++ runtime or names a folder on the build machine: uv run --with pefile python tools/check_exe.py old-town-walk.exe"""

from __future__ import annotations

import re
import sys

import pefile

RUNTIME = ("vcruntime", "msvcp", "ucrtbase", "api-ms-win-crt", "msvcr")


def main() -> int:
    pe = pefile.PE(sys.argv[1])
    imports = sorted(e.dll.decode().lower() for e in pe.DIRECTORY_ENTRY_IMPORT)
    print("imports:", " ".join(imports))
    needs = [d for d in imports if d.startswith(RUNTIME)]
    debug = []
    for entry in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []):
        path = getattr(entry.entry, "PdbFileName", None)
        if path:
            debug.append(path.rstrip(b"\0").decode(errors="replace"))
    data = open(sys.argv[1], "rb").read()
    paths = sorted({m.group().decode() for m in re.finditer(rb"[A-Za-z]:\\[\x20-\x7e]{3,}", data)})
    print("needs runtime:", needs or "none", "| pdb paths:", debug or "none", "| absolute paths:", paths or "none")
    return 1 if needs or debug or paths else 0


if __name__ == "__main__":
    raise SystemExit(main())
