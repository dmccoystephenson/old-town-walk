"""Downloads the raw data once. OSM downloads carry contributor metadata, so they stay out of git: `old-town-walk
sanitize` reduces them to data/raw/osm-sample.osm, and the build reads only that, offline."""

from __future__ import annotations

import hashlib
import json
import time
import urllib.parse
import urllib.request
from pathlib import Path

from .geo import UTM_EPSG, Area, utm_to_lonlat

OSM_MAP = "https://api.openstreetmap.org/api/0.6/map"
DEM_EXPORT = "https://elevation.nationalmap.gov/arcgis/rest/services/3DEPElevation/ImageServer/exportImage"
USER_AGENT = "old_town_walk/0.1 (open data 3D map)"
BLOCK_M = 1000.0   # OSM is fetched in blocks this wide, which keeps each call under the API's node limit
MARGIN_M = 60.0    # roads and buildings that start just outside the area


def _get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=240) as r:
        return r.read()


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fetch(area: Area, raw: Path) -> None:
    raw.mkdir(parents=True, exist_ok=True)
    w, s, e, n = area.bounds()
    sources = {}

    def save(name: str, url: str) -> None:
        path = raw / name
        if not path.exists():
            path.write_bytes(_get(url))
            time.sleep(1.1)
        sources[name] = {"url": url, "sha256": sha256(path), "bytes": path.stat().st_size}

    cols = int((e - w) // BLOCK_M + (1 if (e - w) % BLOCK_M else 0))
    rows = int((n - s) // BLOCK_M + (1 if (n - s) % BLOCK_M else 0))
    for r in range(rows):
        for c in range(cols):
            lon0, lat0 = utm_to_lonlat(w + c * BLOCK_M - MARGIN_M, s + r * BLOCK_M - MARGIN_M)
            lon1, lat1 = utm_to_lonlat(w + (c + 1) * BLOCK_M + MARGIN_M, s + (r + 1) * BLOCK_M + MARGIN_M)
            box = ",".join(f"{v:.6f}" for v in (lon0, lat0, lon1, lat1))
            save(f"osm-{r}-{c}.osm", OSM_MAP + "?bbox=" + box)

    border = 2 * area.cell
    px = int(round((e - w + 2 * border) / area.cell))
    query = {"bbox": f"{w - border},{s - border},{e + border},{n + border}", "bboxSR": UTM_EPSG, "imageSR": UTM_EPSG,
             "size": f"{px},{px}", "format": "tiff", "pixelType": "F32",
             "interpolation": "RSP_BilinearInterpolation", "noDataInterpretation": "esriNoDataMatchAny", "f": "image"}
    save("dem-3dep.tif", DEM_EXPORT + "?" + urllib.parse.urlencode(query))
    (raw / "SOURCES.json").write_text(json.dumps(sources, indent=1) + "\n")


def verify(raw: Path) -> list[str]:
    """Names of raw files that are missing or do not match SOURCES.json."""
    sources = json.loads((raw / "SOURCES.json").read_text())
    return [name for name, s in sources.items() if not (raw / name).exists() or sha256(raw / name) != s["sha256"]]
