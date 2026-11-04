# Notices

The code is under the MIT license (see LICENSE). The data it uses is not.

- **Map data** © OpenStreetMap contributors, under the [Open Database License 1.0](https://opendatacommons.org/licenses/odbl/1-0/).
  See <https://www.openstreetmap.org/copyright>. `data/raw/osm-sample.osm`, and the tiles built from it (in `build/tiles`
  in the Windows download's `tiles` folder, and served to the browser version), are a derivative database. They are offered under the ODbL in this
  repository: `data/raw/osm-sample.osm` is the derived database in full, and `old-town-walk build` rebuilds the tiles from it.
  The sample keeps only geometry, element ids and the tags the build reads.
- **Elevation** courtesy of the U.S. Geological Survey, 3D Elevation Program (`data/raw/dem-3dep.tif`). USGS data is in
  the public domain; attribution is requested.
- **Rendering** by [Strata](https://github.com/Preponderous-Software/strata) (MIT), fetched at build time at the commit
  in `strata.ref`, with the third-party libraries it builds in, such as SDL3 (zlib license). The browser version also serves Strata's web kit scripts.
