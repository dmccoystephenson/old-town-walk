# Notices

The code is under the MIT license (see LICENSE). The data it uses is not.

- **Map data** © OpenStreetMap contributors, under the [Open Database License 1.0](https://opendatacommons.org/licenses/odbl/1-0/).
  See <https://www.openstreetmap.org/copyright>. `data/raw/osm-sample.osm`, and the tiles built from it (in `build/tiles`
  in the Windows download's `tiles` folder, and served to the browser version), are a derivative database. They are offered under the ODbL in this
  repository: `data/raw/osm-sample.osm` is the derived database in full, and `old-town-walk build` rebuilds the tiles from it.
  The sample keeps only geometry, element ids and the tags the build reads.
  `data/raw/street-names.csv` adds the ids and names of the public roads on the map, for the street signs.
  The positions in `data/landmarks.csv` were found from OpenStreetMap and the map's own buildings; its names are of
  public places only, its sentences are this project's own, and each cites the source it was checked against.
- **Elevation** courtesy of the U.S. Geological Survey, 3D Elevation Program (`data/raw/dem-3dep.tif`). USGS data is in
  the public domain; attribution is requested.
- **Lidar** courtesy of the U.S. Geological Survey, 3D Elevation Program: survey USGS_LPC_CO_SoPlatteRiver_Lot2a_2013,
  read from the USGS open lidar bucket on AWS. `data/raw/lidar-ndsm.tif` is a one-metre grid derived from it (heights
  above ground and a leaf share). Public domain; attribution is requested.
- **Roof colours** in `data/raw/roof-colours.csv` are one colour a building, sampled from USDA Farm Service Agency NAIP
  imagery through the USGS National Map. NAIP is in the public domain. The imagery itself is not kept or drawn.
- **Building facts** in `data/building-overrides.csv` (what a wall is made of, how many storeys) are each from the
  public source cited on their row.
- **Rendering** by [Strata](https://github.com/Preponderous-Software/strata) (MIT), fetched at build time at the commit
  in `strata.ref`, with the third-party libraries it builds in, such as SDL3 (zlib license). The browser version also serves Strata's web kit scripts.
- **Historic images in the README** are in the public domain in the United States and are not part of the program:
  - `docs/images/then-1929-looking-west.jpg` and `then-1929-looking-east.jpg`: aerial views of Fort Collins, June 1929,
    photographer unknown, cropped. Colorado State University archives, which state the material is believed to be in
    the public domain ([1](http://hdl.handle.net/10217/22454), [2](http://hdl.handle.net/10217/22455)), through Wikimedia
    Commons ([1](https://commons.wikimedia.org/wiki/File:Fort_Collins_-_aerial_view_(UHPC_5968).jpg),
    [2](https://commons.wikimedia.org/wiki/File:Fort_Collins_-_aerial_view_(UHPC_5969).jpg)).
  - `docs/images/then-1940-intersection.jpg`: postcard, 1940, from the Curt Teich collection at the Newberry Library,
    with no known copyright, trimmed, through
    [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Ft._Collins_CO_-_Business_District,_Showing_Intersection_of_Mountain,_College_Avenues_and_Linden..._(NBY_430572).jpg).
