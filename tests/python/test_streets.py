"""Street names reach the manifest, and every sign stands somewhere sensible."""

from pathlib import Path

import numpy as np

from old_town_walk.geo import Area
from old_town_walk.osm import parse
from old_town_walk.streets import KERB_M, _distance_to_line, _inside, load_names, manifest_lines, short_name

ROOT = Path(__file__).resolve().parents[2]


def built():
    area = Area.load(ROOT / "area.json")
    osm = parse([ROOT / "data/raw/osm-sample.osm"])
    return osm, manifest_lines(area, osm, load_names(ROOT / "data/raw/street-names.csv"))


def test_names_are_written_as_on_a_sign():
    assert short_name("East Mountain Avenue") == "E Mountain Ave"
    assert short_name("North College Avenue") == "N College Ave"
    assert short_name("Linden Street") == "Linden St"
    assert short_name("East Drive") == "East Dr"
    assert short_name("Old Town Square") == "Old Town Square"


def test_the_manifest_names_old_town_and_signs_its_crossings():
    _, lines = built()
    names = [line.split(" ", 2)[2] for line in lines if line.startswith("street ")]
    assert "Linden St" in names and "Walnut St" in names and "N College Ave" in names
    assert sum(line.startswith("road ") for line in lines) >= len(names)
    assert sum(line.startswith("sign ") for line in lines) > 20


def test_no_sign_stands_in_a_building_or_in_the_road():
    osm, lines = built()
    rings = [np.asarray(f.rings[0]) for f in osm.areas if "building" in f.tags]
    roads = [np.asarray(line.split()[2:], dtype=float).reshape(-1, 2) for line in lines if line.startswith("road ")]
    for line in lines:
        if line.startswith("sign "):
            spot = np.array(line.split()[1:3], dtype=float)
            assert not any(_inside(spot, r) for r in rings if np.abs(r - spot).max(axis=1).min() < 60)
            near = [r for r in roads if np.abs(r - spot).max(axis=1).min() < 80]
            assert all(_distance_to_line(spot, r) >= KERB_M - 0.1 for r in near)
