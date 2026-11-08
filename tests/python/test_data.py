"""Checks on this game's committed data, which streetkit's own tests do not see."""

import xml.etree.ElementTree as ET
from pathlib import Path

from streetkit.sanitize import KEEP_TAGS


def test_sanitized_sample_holds_only_allowed_tags():
    from streetkit.sanitize import KEEP_TAGS

    root = ET.parse(Path(__file__).resolve().parents[2] / "data" / "raw" / "osm-sample.osm").getroot()
    assert {t.get("k") for t in root.iter("tag")} <= KEEP_TAGS
    for el in root:
        assert set(el.attrib) <= {"id", "lat", "lon"}
