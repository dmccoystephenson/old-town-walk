"""Writes one tile file per grid square, plus a manifest. app/tile.cpp reads the same layout.

A tile's coordinates are local to its north-west corner, so precision does not depend on where on the map the
tile is; the viewer adds the offset when it loads one. Heights are metres above the manifest's datum.

    tile     := header heights colors mesh* footprint* tree*
    header   := "OTWTILE1" u32 version  i32 ix iy  f32 size cell  i32 samples meshes footprints trees
    heights  := (samples + 2)^2 f32, row by row from the north, with one extra sample all round for normals
    colors   := samples^2 sRGB bytes * 3, padded to 4 bytes
    mesh     := u32 kind  i32 vertices indices  xyz f32  normal i8[3]+pad  colour u8[3]+pad  index u16 or u32, padded
    footprint:= i32 points  x z f32
    tree     := x y z height crown r g b, f32

Indices are u16 when the mesh has fewer than 65536 vertices.
"""

from __future__ import annotations

import hashlib
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from .geo import Area, tile_north_west, tile_of
from .mesh import Mesh, clean_ring, extrude, points_in_ring, ribbon_quads, signed_area
from .osm import OsmData

MAGIC = b"OTWTILE1"
VERSION = 1
HEADER = struct.Struct("<8sIiiffiiii")
TREE = struct.Struct("<8f")
KINDS = {"buildings": 0, "roads": 1, "paths": 2}

GRASS = (112, 142, 84)
AREA_COLOURS = [
    (lambda t: t.get("natural") == "water" or t.get("landuse") in ("reservoir", "basin") or "water" in t, (72, 108, 138)),
    (lambda t: t.get("natural") in ("wood", "scrub") or t.get("landuse") == "forest", (72, 104, 58)),
    (lambda t: t.get("leisure") in ("park", "garden", "pitch", "recreation_ground", "golf_course")
        or t.get("landuse") in ("grass", "recreation_ground", "cemetery", "meadow"), (100, 148, 76)),
    (lambda t: t.get("amenity") in ("parking", "fuel") or t.get("landuse") in ("commercial", "retail", "industrial", "railway", "construction")
        or t.get("place") == "square", (138, 136, 130)),
    (lambda t: t.get("landuse") == "residential", (118, 146, 90)),
]
WALLS = [(186, 150, 124), (160, 96, 76), (205, 195, 176), (222, 212, 190), (150, 120, 100), (176, 168, 156)]
ROOFS = [(112, 96, 88), (96, 96, 104), (128, 112, 98), (88, 82, 80)]
FLAT_ROOF = (128, 126, 122)
ASPHALT, CONCRETE = (66, 66, 70), (186, 182, 174)
ROAD_WIDTHS = {"motorway": 12, "trunk": 11, "primary": 11, "secondary": 10, "tertiary": 9, "unclassified": 7,
               "residential": 7.5, "living_street": 6, "service": 4, "motorway_link": 6, "trunk_link": 6,
               "primary_link": 6, "secondary_link": 6, "tertiary_link": 6}
PATH_WIDTHS = {"footway": 1.8, "path": 1.5, "cycleway": 2.5, "pedestrian": 4, "steps": 1.5, "track": 3}


class Dem:
    """A north-up float32 GeoTIFF in the grid's UTM zone, sampled bilinearly at pixel centres."""

    def __init__(self, path: Path):
        import tifffile
        with tifffile.TiffFile(path) as tif:
            page = tif.pages[0]
            self.z = page.asarray().astype(np.float64)
            scale, tie = page.tags["ModelPixelScaleTag"].value, page.tags["ModelTiepointTag"].value
        bad = self.z < -1000
        if bad.any():
            self.z[bad] = np.median(self.z[~bad])
        self.sx, self.sy, self.e0, self.n0 = float(scale[0]), float(scale[1]), float(tie[3]), float(tie[4])

    def height(self, east, north):
        col = np.clip((np.asarray(east, np.float64) - self.e0) / self.sx - 0.5, 0, self.z.shape[1] - 1.000001)
        row = np.clip((self.n0 - np.asarray(north, np.float64)) / self.sy - 0.5, 0, self.z.shape[0] - 1.000001)
        c, r = np.floor(col).astype(int), np.floor(row).astype(int)
        fc, fr = col - c, row - r
        top = self.z[r, c] * (1 - fc) + self.z[r, c + 1] * fc
        bottom = self.z[r + 1, c] * (1 - fc) + self.z[r + 1, c + 1] * fc
        return top * (1 - fr) + bottom * fr


def pick(options, key: str):
    return options[zlib.crc32(key.encode()) % len(options)]


def parse_length(value: str | None) -> float | None:
    if not value:
        return None
    text = value.strip().lower().replace("m", "").strip()
    try:
        if text.endswith(("'", "ft")):
            return float(text.rstrip("'ft ")) * 0.3048
        return float(text)
    except ValueError:
        return None


def building_shape(tags: dict, area: float) -> tuple[float, float]:
    """Eave height and roof rise, in metres."""
    kind = tags.get("building", "yes")
    house = kind in ("house", "detached", "residential", "semidetached_house", "bungalow") or (kind == "yes" and area < 320)
    height = parse_length(tags.get("height"))
    levels = parse_length(tags.get("building:levels"))
    if height:
        return height, 0.0
    if levels:
        return levels * 3.2 + (0.6 if not house else 0), 2.5 if house else 0.0
    if kind in ("garage", "garages", "shed", "carport"):
        return 2.7, 0.0
    return (4.5, 2.5) if house else (7.0, 0.0)


@dataclass
class Tile:
    ix: int
    iy: int
    meshes: dict = field(default_factory=lambda: {k: Mesh() for k in KINDS})
    footprints: list = field(default_factory=list)   # (N,2) UTM
    trees: list = field(default_factory=list)


def _inside(east: float, north: float, area: Area, keys: set) -> tuple[int, int] | None:
    key = tile_of(east, north, area.tile_size)
    return key if key in keys else None


def build_tiles(area: Area, osm: OsmData, dem: Dem) -> tuple[dict, float]:
    """Assigns each feature to the tile holding its centre. Geometry may cross a tile edge, which hides seams."""
    keys = set(area.tiles())
    tiles = {k: Tile(*k) for k in keys}
    west, south, east, north = area.bounds()
    datum = float(round(float(dem.height((west + east) / 2, (south + north) / 2))))
    ox, on = west, south   # meshes stay in small numbers while they are built

    for f in osm.areas:
        if "building" not in f.tags or f.tags["building"] in ("no", "construction", "proposed", "demolished"):
            continue
        ring = clean_ring(f.rings[0])
        if len(ring) < 3:
            continue
        ce, cn = ring.mean(axis=0)
        key = _inside(ce, cn, area, keys)
        if key is None:
            continue
        ground = dem.height(ring[:, 0], ring[:, 1]) - datum
        base, top = float(ground.min()) - 0.4, float(ground.max())
        eave, rise = building_shape(f.tags, abs(signed_area(ring)))
        local = ring - (ox, on)
        roof = FLAT_ROOF if rise == 0 else pick(ROOFS, f.osm_id + "r")
        extrude(tiles[key].meshes["buildings"], local, base, top + eave, pick(WALLS, f.osm_id), roof, rise)
        tiles[key].footprints.append(ring)

    for f in osm.lines:
        hw = f.tags.get("highway")
        if hw is None or f.tags.get("tunnel") in ("yes", "culvert") or f.tags.get("area") == "yes":
            continue
        is_path = hw in PATH_WIDTHS
        width = parse_length(f.tags.get("width")) or PATH_WIDTHS.get(hw) or ROAD_WIDTHS.get(hw)
        if width is None:
            continue
        kind, rgb, lift = ("paths", CONCRETE, 0.20) if is_path else ("roads", ASPHALT, 0.12)
        for quad in ribbon_quads(f.line, width):
            ce, cn = np.mean(quad, axis=0)
            key = _inside(float(ce), float(cn), area, keys)
            if key is None:
                continue
            pts = np.array(quad)
            ys = dem.height(pts[:, 0], pts[:, 1]) - datum + lift
            corner = [(p[0] - ox, y, -(p[1] - on)) for p, y in zip(pts, ys)]
            a, b, d = (np.array(corner[i]) for i in (0, 1, 3))
            order = corner if np.cross(b - a, d - a)[1] > 0 else corner[::-1]
            tiles[key].meshes[kind].quad(*order, rgb)

    for f in osm.points:
        if f.tags.get("natural") != "tree":
            continue
        key = _inside(*f.point, area, keys)
        if key is None:
            continue
        rgb = pick([(66, 98, 52), (80, 112, 58), (58, 90, 50)], f.osm_id)
        tiles[key].trees.append((f.point[0], f.point[1], parse_length(f.tags.get("height")) or 9.0, rgb))
    return tiles, datum


def terrain_grids(area: Area, tile: Tile, osm_painted: list, dem: Dem, datum: float):
    size, cell = area.tile_size, area.cell
    samples = int(round(size / cell)) + 1
    tw, tn = tile_north_west(tile.ix, tile.iy, size)
    ticks = np.arange(-1, samples + 1) * cell
    gx, gz = np.meshgrid(tw + ticks, tn - ticks)
    heights = dem.height(gx, gz) - datum
    east, north = gx[1:-1, 1:-1], gz[1:-1, 1:-1]
    colours = np.empty((samples, samples, 3), dtype=np.uint8)
    colours[:] = GRASS
    for box, ring, rgb in osm_painted:
        if box[2] < east.min() or box[0] > east.max() or box[3] < north.min() or box[1] > north.max():
            continue
        mask = points_in_ring(east, north, ring)
        colours[mask] = rgb
    return samples, heights.astype(np.float32), colours


def painted_areas(osm: OsmData) -> list:
    out = []
    for f in osm.areas:
        if "building" in f.tags:
            continue
        rgb = next((c for test, c in AREA_COLOURS if test(f.tags)), None)
        if rgb is None:
            continue
        for ring in f.rings:
            out.append((abs(signed_area(ring)), ring, rgb))
    out.sort(key=lambda t: -t[0])
    return [((r[:, 0].min(), r[:, 1].min(), r[:, 0].max(), r[:, 1].max()), r, c) for _, r, c in out]


def write_tile(path: Path, area: Area, tile: Tile, painted: list, dem: Dem, datum: float) -> None:
    samples, heights, colours = terrain_grids(area, tile, painted, dem, datum)
    size = area.tile_size
    tw, tn = tile_north_west(tile.ix, tile.iy, size)
    west, south, _, _ = area.bounds()
    meshes = [(k, m) for k, m in tile.meshes.items() if m.positions]
    pad = lambda n: b"\0" * (-n % 4)
    with open(path, "wb") as f:
        f.write(HEADER.pack(MAGIC, VERSION, tile.ix, tile.iy, size, area.cell, samples, len(meshes),
                            len(tile.footprints), len(tile.trees)))
        f.write(heights.tobytes())
        f.write(colours.tobytes() + pad(colours.size))
        for kind, m in meshes:
            pos = np.array(m.positions, np.float32)
            pos[:, 0] += float(west - tw)
            pos[:, 2] += float(tn - south)
            normal = np.zeros((len(pos), 4), np.int8)
            normal[:, :3] = np.round(np.array(m.normals) * 127).astype(np.int8)
            colour = np.zeros((len(pos), 4), np.uint8)
            colour[:, :3] = np.array(m.colors, np.uint8)
            index = np.array(m.indices, np.uint16 if len(pos) < 65536 else np.uint32)
            f.write(struct.pack("<Iii", KINDS[kind], len(pos), len(index)))
            f.write(pos.tobytes() + normal.tobytes() + colour.tobytes() + index.tobytes() + pad(index.nbytes))
        for ring in tile.footprints:
            f.write(struct.pack("<i", len(ring)))
            f.write(np.column_stack((ring[:, 0] - tw, tn - ring[:, 1])).astype(np.float32).tobytes())
        for east, north, height, rgb in tile.trees:
            y = float(dem.height(east, north)) - datum
            f.write(TREE.pack(east - tw, y, tn - north, height, height * 0.32, *[(c / 255) ** 2.2 for c in rgb]))


def write_manifest(path: Path, area: Area, datum: float, sizes: dict) -> None:
    e, n = area.centre_utm()
    lines = ["old-town-walk-manifest 1", f"name {area.name}", "epsg 32613", f"tile_size {area.tile_size:g}", f"cell {area.cell:g}",
             f"datum {datum:g}", f"latitude {area.centre_lat}", f"start {e:.1f} {n:.1f}", f"tiles {len(sizes)}"]
    lines += [f"tile {ix} {iy} {size} {digest}" for (ix, iy), (size, digest) in sorted(sizes.items())]
    path.write_text("\n".join(lines) + "\n")


def read_manifest(path: Path) -> dict:
    out, tiles = {}, {}
    for line in Path(path).read_text().splitlines():
        word, _, rest = line.partition(" ")
        if word == "tile":
            ix, iy, size, digest = rest.split()
            tiles[(int(ix), int(iy))] = (int(size), digest)
        elif word not in ("old-town-walk-manifest", "tiles"):
            out[word] = rest
    out["tiles"] = tiles
    return out


def tile_name(ix: int, iy: int) -> str:
    return f"{ix}_{iy}.tile"


def build_all(area: Area, osm: OsmData, dem: Dem, out: Path) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    tiles, datum = build_tiles(area, osm, dem)
    painted = painted_areas(osm)
    sizes = {}
    for key, tile in sorted(tiles.items()):
        path = out / tile_name(*key)
        write_tile(path, area, tile, painted, dem, datum)
        sizes[key] = (path.stat().st_size, hashlib.sha256(path.read_bytes()).hexdigest()[:16])
    write_manifest(out / "manifest.txt", area, datum, sizes)
    return {"tiles": len(tiles), "buildings": sum(len(t.footprints) for t in tiles.values()),
            "trees": sum(len(t.trees) for t in tiles.values()), "bytes": sum(s for s, _ in sizes.values())}
