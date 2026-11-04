# Old Town Walk

Walk or fly through 2 km by 2 km of Old Town Fort Collins, Colorado, in 3D. It is built only from open map and
elevation data and rendered with [Strata](https://github.com/Preponderous-Software/strata).

![Low flight over Old Town Fort Collins: streets, sidewalks and blocks of buildings](docs/images/street.png)

*Rendered headless by the real viewer. Map data © OpenStreetMap contributors (ODbL); elevation courtesy of the USGS.*

## Download and run

On Windows, download `old-town-walk-windows.zip` from the releases page, unzip it, and double-click `old-town-walk.exe`.
There is nothing to install. It needs OpenGL 4.5, which most PCs from the last decade have.

The program is not code-signed, so Windows SmartScreen may warn about an unknown publisher. Choose **More info**, then
**Run anyway**.

## Controls

| Input | Action |
|---|---|
| W A S D | walk (**Shift** runs) |
| mouse or arrow keys | look |
| F | fly, and F again to land outside any building |
| W A S D while flying | fly where you look at 20 m/s (**Shift** 80 m/s) |
| E or Space, Q or Ctrl | climb, descend |
| `[` `]` | time of day |
| P | save a screenshot |
| H | hide the on-screen label |
| Esc | free the mouse (click to take it back); Esc again quits |

Flying stops at the edge of the map, up to 400 m above the ground.

## Building it

Needs Python 3.12 via [uv](https://docs.astral.sh/uv/), CMake ≥ 3.24, a C++17 compiler and OpenGL 4.5. The build is
offline apart from fetching Strata.

```sh
uv run old-town-walk verify     # the data matches data/raw/SOURCES.json
uv run old-town-walk build      # -> build/tiles/
cmake -S . -B build/app && cmake --build build/app -j4
./build/app/old-town-walk       # finds build/tiles by itself; --tiles DIR picks another folder
```

To refresh the data, `uv run old-town-walk fetch` downloads it into `build/fetched`, which is never committed, because a
full OpenStreetMap download carries contributor names and business contact details. `uv run old-town-walk sanitize` then
reduces it to the geometry and the few tags the build reads.

Headless, with `LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe` where there is no GPU:

```sh
./build/app/old-town-walk --headless --yaw 90 --walk-ahead 30 --screenshot out.png
./build/app/old-town-walk --headless --radius 300 --pass-east -900 --bench 20
```

## How the tiles work

- **The grid is global.** A tile is a 500 m square of UTM zone 13N, named by `floor(east / 500)` and
  `floor(north / 500)`. Its name never depends on which area was built, so areas can grow without renaming.
- **Tiles are self-contained.** Positions are relative to the tile's north-west corner, and the viewer adds the offset
  on load. Precision stays the same wherever the tile is, which suits 32-bit floats.
- **No seams.** Neighbours share their edge samples, and each height grid has one extra row all round so normals match
  across the edge. A feature belongs to the tile holding its centre, and may overhang into the next tile.
- **Streaming is a pure function.** `app/streaming.h` takes the position and what is loaded, and returns what to load
  and unload. A tile loads within the load radius, stays until 200 m beyond it, and the budget keeps Strata under its
  limit of 256 meshes at four meshes a tile.
- **Terrain is a static mesh per tile**, not `strata_set_terrain`, because Strata holds one terrain tile at a time.

The layout is documented at the top of `old_town_walk/tiles.py`. `manifest.txt` lists every tile with its size and hash.

## Limits

- Tiles load on the main thread, one a frame, and there is no level of detail.
- Only four-cornered footprints get a gabled roof, and holes in buildings are ignored.
- Building heights are mostly defaults, because few buildings are tagged with one.
- Water is only a colour, you can walk on it, and bridges are not modelled.

## License

The code is MIT (see `LICENSE`). The data is not: see `NOTICE.md`. Map data © OpenStreetMap contributors, under the
Open Database License. Elevation courtesy of the U.S. Geological Survey.
