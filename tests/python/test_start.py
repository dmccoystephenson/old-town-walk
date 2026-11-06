"""The walk begins in the open, not in or against a building."""

import re
from pathlib import Path

import numpy as np

from old_town_walk.geo import Area
from old_town_walk.osm import parse

ROOT = Path(__file__).resolve().parents[2]
CLEARANCE_M = 10.0


def start_offset():
    text = (ROOT / "app" / "walker.h").read_text()
    east = re.search(r"kStartEast = (-?[\d.]+)", text).group(1)
    north = re.search(r"kStartNorth = (-?[\d.]+)", text).group(1)
    return float(east), float(north)


def distance_to_ring(point, ring):
    a, b = ring[:-1], ring[1:]
    d = b - a
    length = np.where((d * d).sum(1) > 0, (d * d).sum(1), 1)
    t = np.clip(((point - a) * d).sum(1) / length, 0, 1)
    return float(np.hypot(*(a + d * t[:, None] - point).T).min())


def inside(point, ring):
    x, y = point
    hit = False
    for (ax, ay), (bx, by) in zip(ring[:-1], ring[1:]):
        if (ay > y) != (by > y) and x < (bx - ax) * (y - ay) / (by - ay) + ax:
            hit = not hit
    return hit


def test_the_start_is_clear_of_every_building():
    east, north = start_offset()
    centre = np.array(Area.load(ROOT / "area.json").centre_utm())
    point = centre + [east, north]
    rings = [np.asarray(f.rings[0]) for f in parse([ROOT / "data/raw/osm-sample.osm"]).areas if "building" in f.tags]
    near = [r for r in rings if np.abs(r - point).max(axis=1).min() < 200]
    assert near, "no buildings near the start: the offset or the map has moved"
    assert not any(inside(point, r) for r in near)
    assert min(distance_to_ring(point, r) for r in near) >= CLEARANCE_M
