# Old Town Walk

Walk or fly through 2 km by 2 km of Old Town Fort Collins, Colorado, in 3D. It is built only from open map and
elevation data and rendered with [Strata](https://github.com/Preponderous-Software/strata).

![Old Town Fort Collins from 95 m: the street grid and blocks of buildings at evening](docs/images/old-town.png)

![A tree-lined street in Old Town seen from a low flight](docs/images/street.png)

*Rendered by the real viewer. Map data © OpenStreetMap contributors (ODbL); elevation courtesy of the USGS.*

## Then and now

Old Town in old photographs, beside roughly the same view in the viewer today. The map shows the streets and buildings
of now, so the rail yards and streetcars are gone, and the views are matched by hand rather than exactly. The foothills
are missing because the map stops 2 km out.

| Then | Now |
|---|---|
| ![1929 aerial photograph looking west over the Cache la Poudre River, with Linden Street leading into town and the foothills beyond](docs/images/then-1929-looking-west.jpg) | ![The viewer looking west toward downtown along Linden Street](docs/images/now-looking-west.jpg) |
| **1929.** Looking west over the river, with Linden Street leading into town. | **Today, in the viewer.** |
| ![1929 aerial photograph looking east over downtown, with rail yards in the foreground and Mountain Avenue along the right](docs/images/then-1929-looking-east.jpg) | ![The viewer looking east over the same part of downtown](docs/images/now-looking-east.jpg) |
| **1929.** Looking east over downtown, with Mountain Avenue along the right. | **Today, in the viewer.** |
| ![1940 postcard of a streetcar at the intersection of Mountain and College Avenues and Linden Street](docs/images/then-1940-intersection.jpg) | ![The viewer at the intersection of Mountain and College Avenues and Linden Street](docs/images/now-intersection.jpg) |
| **1940 postcard.** Mountain and College Avenues at Linden Street. | **Today, in the viewer.** Same intersection, camera angle approximate. |

*The 1929 photographs are from the Colorado State University archives and the postcard is from the Curt Teich collection at
the Newberry Library. All three are in the public domain in the United States; `NOTICE.md` has sources.*

## Download and run

On Windows, download `old-town-walk-windows.zip` from the releases page, unzip it, and double-click `old-town-walk.exe`.
There is nothing to install. It needs OpenGL 4.5, which most PCs from the last decade have.

The program is not code-signed, so Windows SmartScreen may warn about an unknown publisher. Choose **More info**, then
**Run anyway**.

Street signs stand at the crossings, and the status panel names the street you are on.

The bike rides like one: it builds up speed, coasts, turns in arcs, and cannot move sideways.

## Play in a browser

Play it now at **[old-town-walk.play.danielstephenson.dev](https://old-town-walk.play.danielstephenson.dev)**.

The page in `web/` runs the same tiles and walker on Strata's WebGL 2 backend and web kit. It needs WebGL 2 and works
with a keyboard and mouse or a touch screen: a stick to walk, drag to look, and buttons to fly, climb and descend.

```sh
source /path/to/emsdk/emsdk_env.sh     # emsdk 6.0.11, Strata's pin
uv run old-town-walk build
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web -j8
uv run python tools/build_site.py build-web build/tiles web-site
python3 -m http.server 8000 --directory web-site
```

Tiles are downloaded as they are needed, gzipped, and the page unpacks them, so the map costs about 3 MB to start and
11 MB if you walk all of it. The page stores nothing beyond having shown the controls once, and reports nothing.
`web/smoke/smoke.cjs` plays it in headless Chromium, on a desktop and a phone profile, with real input.

## Controls

Press **H** in the window to see these at any time. In a browser on a phone, the on-screen stick and buttons do the same.

| Key | What it does |
|---|---|
| WASD | Move. On foot you stop at walls; in the air you follow your view. |
| Mouse or arrow keys | Look |
| Shift | Go faster: run, fly four times as fast, or sprint on the bike. The panel shows the speed you are going. |
| Space | Jump. In the air, Space climbs. |
| F | Fly, or land on the ground outside any building |
| E or Space, Q or Ctrl | Climb and descend while flying |
| Mouse wheel, or Page Up and Page Down | Set your speed: 5 to 800 m/s flying, 0.7 to 14 m/s on foot |
| `-` and `=` | Shorten or lengthen the view, 200 to 1,500 m |
| `,` and `.` | Mouse sensitivity |
| 9 and 0 | Field of view, 40 to 110 degrees |
| I | Invert the mouse's up and down |
| Z | Hold to zoom in on something far away |
| B | Get on or off the bike: W pedals, S brakes, A and D steer, Shift goes faster |
| R | Back to where you started |
| `[` and `]` | Time of day |
| P | Save a screenshot |
| Tab | Hide the status panel |
| Backspace | Put every setting back to its default |
| Esc | Free the mouse. A click takes it back, and Esc again quits. |

Your settings are remembered between runs, in `old-town-walk/settings.txt` under your user configuration folder.
`old-town-walk --help` lists the command-line options.

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
