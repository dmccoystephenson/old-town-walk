"""The site tool refuses a build that names the machine it was built on."""

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("build_site", ROOT / "tools" / "build_site.py")
build_site = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_site)


def make_build(tmp_path, wasm=b"\0asm", glue=b"var x;"):
    build = tmp_path / "build-web"
    kit = build / "_deps" / "strata-src" / "web" / "kit"
    kit.mkdir(parents=True)
    (build / "walk.wasm").write_bytes(wasm)
    (build / "walk.js").write_bytes(glue)
    (kit / "strata-kit.js").write_text("//")
    (kit / "strata-loader.js").write_text("//")
    tiles = tmp_path / "tiles"
    tiles.mkdir()
    (tiles / "manifest.txt").write_text("old-town-walk-manifest 1\n")
    (tiles / "1_2.tile").write_bytes(b"OTWTILE1" * 100)
    return build, tiles


def run(build, tiles, out):
    return build_site.main(["build_site", str(build), str(tiles), str(out)])


def test_a_clean_build_gets_gzipped_tiles_and_the_real_wasm_size(tmp_path):
    build, tiles = make_build(tmp_path)
    out = tmp_path / "site"
    assert run(build, tiles, out) == 0
    assert (out / "tiles" / "1_2.tile.gz").read_bytes()[:2] == b"\x1f\x8b"
    assert not (out / "tiles" / "1_2.tile").exists()
    page = (out / "page.js").read_text()
    assert "__WASM_BYTES__" not in page and "bytes: 4," in page


def test_build_paths_are_refused(tmp_path):
    for leak in (b"/home/someone/x.cpp", b"/root/src", b"C:\\Users\\someone"):
        build, tiles = make_build(tmp_path / leak.hex(), wasm=b"\0asm" + leak)
        assert run(build, tiles, tmp_path / leak.hex() / "site") == 1


def test_emscriptens_virtual_home_is_not_a_leak(tmp_path):
    build, tiles = make_build(tmp_path, glue=b'FS.mkdir("/home/web_user")')
    assert run(build, tiles, tmp_path / "site") == 0
