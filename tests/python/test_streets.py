"""Street names and places reach the manifest, and every sign and place stands somewhere sensible."""

import re
from pathlib import Path

import numpy as np
import pytest

from old_town_walk.geo import Area
from old_town_walk.osm import parse
from old_town_walk.streets import KERB_M, _distance_to_line, _inside, load_names, load_places, manifest_lines, place_lines, short_name
from old_town_walk.tiles import read_manifest, write_manifest

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


def places():
    return load_places(ROOT / "data/landmarks.csv")


def test_the_landmarks_are_a_short_list_with_a_source_each():
    rows = places()
    assert 8 <= len(rows) <= 14
    assert len({row["name"] for row in rows}) == len(rows)
    for row in rows:
        assert row["sentence"].endswith(".") and len(row["sentence"]) <= 170
        assert row["source"].startswith("https://")


def test_the_landmarks_carry_no_address_or_contact_details():
    for row in places():
        words = row["name"] + " " + row["sentence"]
        assert not re.search(r"\d+ (North |South |East |West |N |S |E |W )?[A-Z][a-z]+ (St|Street|Ave|Avenue)\b", words)
        assert not re.search(r"\d{3}[-. ]\d{4}|@|www\.|https?:", words)


def test_every_landmark_reaches_the_manifest_and_stands_on_the_map():
    area = Area.load(ROOT / "area.json")
    lines = place_lines(area, places())
    assert len(lines) == len(places())
    west, south, east, north = area.bounds()
    for line, row in zip(lines, places()):
        word, e, n, words = line.split(" ", 3)
        assert word == "place" and words == f"{row['name']}|{row['sentence']}"
        assert west <= float(e) <= east and south <= float(n) <= north


def test_no_landmark_stands_in_a_building():
    area = Area.load(ROOT / "area.json")
    osm = parse([ROOT / "data/raw/osm-sample.osm"])
    rings = [np.asarray(f.rings[0]) for f in osm.areas if "building" in f.tags]
    for line in place_lines(area, places()):
        spot = np.array(line.split(" ", 3)[1:3], dtype=float)
        assert not any(_inside(spot, r) for r in rings if np.abs(r - spot).max(axis=1).min() < 60), line


def test_a_place_outside_the_map_is_left_out():
    area = Area.load(ROOT / "area.json")
    assert place_lines(area, [{"name": "Far Off", "lat": "39.7", "lon": "-104.9", "sentence": "Elsewhere."}]) == []


@pytest.mark.parametrize("name", ['A "quoted" place', "A|B", "Caf\u00e9", "back\\slash"])
def test_a_place_must_be_plain_text(name):
    with pytest.raises(ValueError):
        place_lines(Area.load(ROOT / "area.json"), [{"name": name, "lat": "40.5877", "lon": "-105.0761", "sentence": "Words."}])


def test_a_place_line_survives_the_manifest(tmp_path):
    area = Area.load(ROOT / "area.json")
    path = tmp_path / "manifest.txt"
    write_manifest(path, area, 1500, {(1, 2): (3, "abc")}, ["place 1.0 2.0 Old Town Square|A sentence."])
    assert read_manifest(path)["place"] == "1.0 2.0 Old Town Square|A sentence."
