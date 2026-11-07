/* Old Town Walk's page: boots the engine behind Strata's loading shell, downloads the tiles the player
   is near, and turns keys, the mouse and fingers into the walker's input. Keeps your settings in this
   browser only, and reports nothing. */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const canvas = $("view");
  const MORE = {href: "https://danielstephenson.dev/play", text: "More games"};
  const PLACE = {lat: 40.5877, lon: -105.0761};  // Old Town, for where the sun stands
  const MIDSUMMER_AFTERNOON = Date.UTC(2026, 6, 15, 21, 0) / 1000;
  const MOUSE_DEG = 0.15, TOUCH_DEG = 0.3, KEY_DEG = 90, MAX_MOUSE_STEP = 300, CONCURRENT = 3, RETRIES = 3, RETRY_MS = 5000, RETRY_MAX_MS = 60000;
  const search = new URLSearchParams(location.search);
  const coarse = matchMedia("(pointer: coarse)").matches || search.get("touch") === "1";
  const stored = StrataKit.store;
  // Whoever has asked their system for less motion gets a level view on the bike, from the moment they ask.
  const lessMotion = matchMedia("(prefers-reduced-motion: reduce)");
  lessMotion.addEventListener("change", () => { if (lessMotion.matches) canvas.style.transform = ""; });


  const FLY = [5, 800], WALK = [0.7, 14], BIKE = [3, 40], VIEW = [200, 1500], NOTCH = 1.25, VIEW_STEP = 100;
  const SENSITIVITY = [0.03, 0.6];
  const FOV = [40, 110], FOV_STEP = 5, ZOOMED = 0.3;
  const clamp = (v, [low, high]) => Math.min(high, Math.max(low, v));
  const DEFAULTS = {fly: 20, walk: 1.4, ride: 12, view: coarse ? 500 : 700, sensitivity: MOUSE_DEG, fov: 70, invert: false, status: true};
  DEFAULTS.map = !coarse || Math.min(innerWidth, innerHeight) >= 360;  // the smallest phones start with the view to themselves
  const settings = {...DEFAULTS};
  function loadSettings() {
    let saved = {};
    try { saved = JSON.parse(stored.get("old-town-walk-settings") || "{}") || {}; } catch (e) { /* start from the defaults */ }
    const number = (v, fallback) => (typeof v === "number" && isFinite(v) ? v : fallback);
    settings.fly = clamp(number(saved.fly, DEFAULTS.fly), FLY);
    settings.walk = clamp(number(saved.walk, DEFAULTS.walk), WALK);
    settings.ride = clamp(number(saved.ride, DEFAULTS.ride), BIKE);
    settings.view = clamp(number(saved.view, DEFAULTS.view), VIEW);
    settings.sensitivity = clamp(number(saved.sensitivity, DEFAULTS.sensitivity), SENSITIVITY);
    settings.fov = clamp(number(saved.fov, DEFAULTS.fov), FOV);
    settings.invert = saved.invert === true;
    settings.status = saved.status !== false;
    settings.map = typeof saved.map === "boolean" ? saved.map : DEFAULTS.map;
  }
  function keepSettings() {
    stored.set("old-town-walk-settings", JSON.stringify(settings));
    showSettings();
  }
  function applySettings() {
    document.body.classList.toggle("no-status", !settings.status);
    document.body.classList.toggle("map-on", settings.map);
    $("mapBtn").setAttribute("aria-pressed", String(settings.map));
    if (!mod) return;
    mod._otw_set_speeds(settings.fly, settings.walk);
    mod._otw_set_bike_speed(settings.ride);
    mod._otw_set_radius(parseFloat(search.get("reach")) || settings.view);
  }

  let mod = null, kit = null, sunTime = MIDSUMMER_AFTERNOON, flying = false, riding = false, shown = false, failed = false;

  /* ------------------------------------------------------- tiles */
  const queue = [];
  let downloading = 0, tilesIn = 0;

  async function unpack(buffer) {
    const b = new Uint8Array(buffer);
    if (b.length < 2 || b[0] !== 0x1f || b[1] !== 0x8b) return b;  // something on the way already unpacked it
    const stream = new Blob([b]).stream().pipeThrough(new DecompressionStream("gzip"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }

  async function download(ix, iy) {
    for (let attempt = 0; attempt < RETRIES; attempt++) {
      try {
        const r = await fetch(`tiles/${ix}_${iy}.tile.gz`);
        if (!r.ok) throw new Error("HTTP " + r.status);
        const bytes = await unpack(await r.arrayBuffer());
        const at = mod._malloc(bytes.length);
        mod.HEAPU8.set(bytes, at);
        const ok = mod._otw_tile(ix, iy, at, bytes.length);
        mod._free(at);
        if (ok) { tilesIn++; return true; }
      } catch (e) {
        console.warn(`tile ${ix} ${iy}: ${e.message}`);
      }
      await new Promise((resolve) => setTimeout(resolve, 400 * (attempt + 1)));
    }
    return false;
  }

  // A tile that never arrives is an invisible wall, so keep asking, less often the longer it goes on.
  const missing = new Map();
  function tryAgainLater(ix, iy) {
    const key = `${ix}_${iy}`, tries = missing.get(key) || 0;
    missing.set(key, tries + 1);
    note("Part of the map did not download. Trying again.");
    setTimeout(() => { queue.push([ix, iy]); pump(); }, Math.min(RETRY_MS * 2 ** tries, RETRY_MAX_MS));
  }

  function pump() {
    while (downloading < CONCURRENT && queue.length) {
      const [ix, iy] = queue.shift();
      downloading++;
      download(ix, iy).then((ok) => {
        downloading--;
        if (!ok && !shown && !failed) { failed = true; StrataLoader.fallback("network", "The map did not download."); }
        if (!ok && shown) tryAgainLater(ix, iy);
        if (ok && missing.delete(`${ix}_${iy}`) && missing.size === 0) note("The missing part of the map has arrived");
        pump();
      });
    }
  }

  /* ------------------------------------------------------- input */
  const held = new Set();
  const move = {ahead: 0, right: 0, run: false};  // the stick
  const lift = {up: false, down: false};
  let dyaw = 0, dpitch = 0, lookId = null, lookX = 0, lookY = 0, settling = 0, zoom = 1;

  const keyed = (a, b) => (held.has(a) ? 1 : 0) - (held.has(b) ? 1 : 0);
  const locked = () => document.pointerLockElement === canvas;

  // Zoomed in, the same hand movement turns less, as on the desktop.
  function look(dx, dy, deg) { dyaw += dx * deg * zoom; dpitch -= (settings.invert ? -dy : dy) * deg * zoom; }

  const touching = () => document.body.classList.contains("touch");
  function setFlying(on) {
    flying = on;
    document.body.classList.toggle("flying", on);
    $("flyBtn").setAttribute("aria-pressed", String(on));
    showSettings();
  }
  function toggleFly() {
    if (!mod) return;
    setFlying(mod._otw_toggle_fly() === 1);
    setRiding(false);
    if (flying) note(touching() ? "Flying: \u25B2 climbs, \u25BC descends, Fly lands" : "Flying: E climbs, Q descends, F lands");
    else note("Landed");
  }
  function setRiding(on) {
    riding = on;
    document.body.classList.toggle("riding", on);
    $("bikeBtn").setAttribute("aria-pressed", String(on));
    if (!on) canvas.style.transform = "";
    showSettings();
  }
  function toggleBike() {
    if (!mod) return;
    const on = mod._otw_toggle_bike() === 1;
    setFlying(false);
    setRiding(on);
    if (!on) note("Off the bike");
    else note(touching() ? "On the bike: push the stick up to pedal, sideways to steer" : "On the bike: W pedals, S brakes, A and D steer");
  }

  function started() {  // the panel is now a pause, not a welcome
    $("start").textContent = "Back to the walk";
    $("homeBtn").hidden = false;
    $("shareBtn").hidden = false;
    $("settings").hidden = false;
    $("places").hidden = $("placeList").childElementCount === 0;
  }
  const helpOpen = () => $("help").classList.contains("on");
  function setHelp(on) {
    $("help").classList.toggle("on", on);
    if (on) {
      showSettings();
      if (locked()) document.exitPointerLock();  // the panel's buttons need a cursor
      $("start").focus();
    } else {
      stored.set("old-town-walk-seen", "1");
      clearShared();
      started();
      canvas.focus();
      grabMouse();
    }
  }

  function shiftSun(hours) {
    sunTime += hours * 3600;
    if (kit) kit.setSun(sunTime, PLACE.lat, PLACE.lon);
    showSettings();
  }

  const formatSpeed = (speed) => speed.toFixed(speed < 10 ? 1 : 0);
  const speedMode = () => (flying ? {key: "fly", range: FLY, name: "Flying speed"} :
    riding ? {key: "ride", range: BIKE, name: "Bike top speed"} : {key: "walk", range: WALK, name: "Walking speed"});
  function changeSpeed(notches) {
    const {key, range, name} = speedMode(), before = settings[key];
    settings[key] = clamp(before * Math.pow(NOTCH, notches), range);
    if (settings[key] === before) note(notches > 0 ? "That is the top speed" : "That is the lowest speed");
    else note(`${name} ${formatSpeed(settings[key])} m/s`);
    applySettings();
    keepSettings();
  }
  function changeView(steps) {
    const before = settings.view;
    settings.view = clamp((Math.round(before / VIEW_STEP) + steps) * VIEW_STEP, VIEW);
    if (settings.view === before) note(steps > 0 ? "That is the longest view" : "That is the shortest view");
    else note(`View ${settings.view} m`);
    applySettings();
    keepSettings();
  }
  function changeFov(steps) {
    const before = settings.fov;
    settings.fov = clamp(before + steps * FOV_STEP, FOV);
    if (settings.fov === before) note(steps > 0 ? "That is the widest view" : "That is the narrowest view");
    else note(`Field of view ${settings.fov}`);
    keepSettings();
  }
  function changeSensitivity(notches) {
    settings.sensitivity = clamp(settings.sensitivity * Math.pow(NOTCH, notches), SENSITIVITY);
    note(`Mouse sensitivity ${settings.sensitivity.toFixed(2)}`);
    keepSettings();
  }
  function resetSettings() {  // a second go within the undo window puts everything back
    const undo = performance.now() < undoUntil ? beforeReset : null;
    beforeReset = undo ? null : {...settings};
    Object.assign(settings, undo || DEFAULTS);
    undoUntil = undo ? 0 : performance.now() + NOTE_MS;
    clearTimeout(undoTimer);
    if (!undo) undoTimer = setTimeout(() => { undoUntil = 0; showSettings(); }, NOTE_MS);
    applySettings();
    keepSettings();
    note(undo ? "Your settings are back" : `Settings back to the defaults${helpOpen() ? "" : ": Backspace again to undo"}`);
  }
  function goToStart() {
    if (!mod) return;
    mod._otw_go_to_start();
    setFlying(false);
    setRiding(false);
    note("Back at the start");
  }

  const clockMinutes = () => Math.floor(((sunTime / 60 - 360) % 1440 + 1440) % 1440);  // Mountain Daylight Time
  function clockTime() {
    const minutes = clockMinutes();
    return `${String(Math.floor(minutes / 60)).padStart(2, "0")}:${String(minutes % 60).padStart(2, "0")}`;
  }

  // The panel draws over the messages, so its controls have to say for themselves where they stand. A
  // button at the end of its range stays focusable: a disabled one would drop the keyboard's place.
  function showSettings() {
    const {key, range, name} = speedMode();
    const atEnds = (down, up, value, [low, high]) => {
      $(down).setAttribute("aria-disabled", String(value <= low));
      $(up).setAttribute("aria-disabled", String(value >= high));
    };
    $("speedName").textContent = name;
    $("speedValue").textContent = `${formatSpeed(settings[key])} m/s`;
    atEnds("speedDown", "speedUp", settings[key], range);
    $("viewValue").textContent = `${settings.view} m`;
    atEnds("viewDown", "viewUp", settings.view, VIEW);
    $("timeValue").textContent = clockTime();
    $("sensitivityValue").textContent = settings.sensitivity.toFixed(2);
    atEnds("sensitivityDown", "sensitivityUp", settings.sensitivity, SENSITIVITY);
    $("resetBtn").textContent = performance.now() < undoUntil ? "Undo reset" : "Reset to defaults";
  }

  // Goes to a spot in manifest eastings and northings. mode: 0 on foot, 1 on the bike, 2 flying. False when it is off the map.
  function travel({east, north, yaw, pitch = 0, mode = 0, height = 60}) {
    if (!mod) return false;
    if (!mod._otw_travel(east, north, yaw === undefined ? status().yaw : yaw, pitch, mode, height)) return false;
    setFlying(mode === 2);
    setRiding(mode === 1);
    return true;
  }

  // A link to a spot: short keys for east, north, yaw, pitch, height, mode (as in travel) and the minute of the day. It is
  // made only when asked for, and nothing about it is kept. Only numbers are read back, each in its range or not at all.
  const LINK = {e: [100000, 900000], n: [0, 10000000], y: [0, 360], p: [-85, 85], h: [0, 400], m: [0, 2], t: [0, 1439]};
  function spotLink(s) {
    // Down, not to the nearest metre: a tile owns its west and south edges, so a spot at the very edge of the map stays on it.
    const spot = new URLSearchParams({e: Math.floor(s.east), n: Math.floor(s.north), y: Math.round(s.yaw) % 360, p: Math.round(s.pitch),
      h: clamp(Math.round(s.height), LINK.h), m: s.riding ? 1 : s.flying ? 2 : 0, t: Math.round(clockMinutes() / 5) * 5 % 1440});
    return `${location.origin}${location.pathname}?${spot}`;
  }
  function readLink() {  // null without a link, false for one that cannot be believed
    if (!search.has("e") && !search.has("n")) return null;
    const spot = {};
    for (const [key, [low, high]] of Object.entries(LINK)) {
      const text = search.get(key);
      if (text === null) continue;
      const value = /^-?\d{1,8}(\.\d{1,3})?$/.test(text) ? Number(text) : NaN;
      if (!(value >= low && value <= high)) return false;
      spot[key] = value;
    }
    return spot.e !== undefined && spot.n !== undefined && (spot.m === undefined || Number.isInteger(spot.m)) && spot;
  }
  function arriveFromLink() {
    const spot = readLink();
    if (spot === null) return;
    if (!spot) { say("That link could not be read, so this is the usual start"); return; }
    if (!travel({east: spot.e, north: spot.n, yaw: spot.y, pitch: spot.p, height: spot.h, mode: spot.m})) {
      say("That link's spot is off the map, so this is the usual start");
      return;
    }
    if (spot.t !== undefined) shiftSun((spot.t - clockMinutes()) / 60);
  }

  // The panel draws over the messages, so with it open the answer goes in the panel.
  function say(text) {
    if (!helpOpen()) { note(text); return; }
    $("shareSaid").textContent = text;
    $("shareSaid").scrollIntoView({block: "nearest"});
  }
  function clearShared() {
    $("shareSaid").textContent = "";
    $("shareLink").hidden = true;
    $("shareLink").value = "";
  }
  // Copying can be refused, in the arcade's frame above all. The page cannot be selected, so the link goes in a field that can.
  function showLink(link) {
    if (!helpOpen()) setHelp(true);
    const field = $("shareLink");
    field.value = link;
    field.hidden = false;
    say("The link could not be copied from here. It is in the box below.");
    field.focus();
    field.setSelectionRange(0, link.length);
  }
  async function share() {
    const s = status();
    if (s.arriving) { say("Wait until you have arrived"); return; }
    const link = spotLink(s);
    clearShared();
    if (touching() && navigator.share) {
      try {
        await navigator.share({title: "Old Town Walk", url: link});
        say("Link shared");
        return;
      } catch (e) {
        if (e.name === "AbortError") return;  // the player changed their mind
      }
    }
    try {
      await navigator.clipboard.writeText(link);
      say("Link copied");
    } catch (e) {
      showLink(link);
    }
  }

  // The manifest's named places, as buttons in the panel. Coming to one, however you got there, shows its sentence.
  const placeSentences = new Map();
  let arrivalsSeen = 0, placeWaiting = "";
  function listPlaces(manifest) {
    for (const line of manifest.split("\n")) {
      const found = /^place (\S+) (\S+) ([^|]+)\|(.*)$/.exec(line);
      if (!found) continue;
      const [, east, north, name, sentence] = found;
      placeSentences.set(name, sentence);
      const button = document.createElement("button");
      button.textContent = name;
      button.addEventListener("click", () => {
        if (travel({east: parseFloat(east), north: parseFloat(north)})) setHelp(false);
        else note("That place is not on the map");
      });
      $("placeList").append(button);
    }
  }
  function showPlace(s) {
    if (s.arrivals !== arrivalsSeen) {
      arrivalsSeen = s.arrivals;
      placeWaiting = `${s.place}: ${placeSentences.get(s.place) || ""}`;
    }
    // Waits its turn, as the panel draws over the messages and the last one may not have been read yet.
    if (placeWaiting && !helpOpen() && performance.now() > noteFree) {
      note(placeWaiting, PLACE_NOTE_MS);
      placeWaiting = "";
    }
  }

  const COMPASS = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"];
  let sinceStatus = 1, frames = 0, fps = 0;
  function showStatus(dt) {
    frames++;
    sinceStatus += dt;
    if (sinceStatus < 0.25) return;
    fps = frames / sinceStatus;
    frames = 0; sinceStatus = 0;
    const s = status();
    showPlace(s);
    const speed = s.pace;
    const heading = COMPASS[Math.floor(s.yaw / 45 + 0.5) & 7];
    const lines = [`${riding ? "riding" : flying ? "flying" : s.hurrying ? "running" : "on foot"}  ${formatSpeed(speed)} m/s`, `street  ${s.street || "-"}`];
    if (!touching()) {
      lines.push(`height  ${Math.round(s.height)} m`, `view    ${settings.view} m`, `facing  ${heading}`, `clock   ${clockTime()}`,
                 `tiles   ${s.tiles}${s.waiting > 0 ? ` +${s.waiting} loading` : ""}`, `${Math.round(fps)} fps  H help`);
    }
    $("status").textContent = lines.join("\n");
  }

  const NOTE_MS = 4000, PLACE_NOTE_MS = 8000;
  let noteTimer = 0, noteFree = 0, beforeReset = null, undoUntil = 0, undoTimer = 0;
  function note(text, ms = NOTE_MS) {
    const n = $("note");
    n.textContent = text;
    n.style.display = "block";
    noteFree = performance.now() + ms;
    clearTimeout(noteTimer);
    noteTimer = setTimeout(() => { n.style.display = "none"; }, ms);
  }

  // The canvas alone, so none of the page's panels are in the picture. Taken in the kit's after hook, straight after
  // a draw, so it is the view as it stands, and a lost context, which draws nothing, cannot save a blank picture.
  let shotWanted = false, shots = 0;
  function takeShot() {
    if (!shotWanted) return;
    shotWanted = false;
    const name = `old-town-walk-${String(shots++).padStart(3, "0")}.png`;
    canvas.toBlob((blob) => {
      if (!blob) { note("The picture could not be saved"); return; }
      const link = document.createElement("a");
      link.href = URL.createObjectURL(blob);
      link.download = name;
      link.click();
      setTimeout(() => URL.revokeObjectURL(link.href), 60000);
      note(`Saved ${name}`);
    }, "image/png");
  }

  /* ------------------------------------------------------- the map */
  // North is always up: only the arrow turns. The roads are drawn once for each size the map takes, to a canvas of
  // their own, and after that the only work is copying them back under the arrow, and only when the arrow has moved.
  const MAP_TICK = 0.1, MAP_SLOP = 12, MAP_SETTLE_MS = 350, MAP_INSET = 3, MAP_CREDIT = 28, MAP_LABEL_GAP = 160, MAP_MAX_DPR = 2;
  const mapView = $("map"), mapPen = mapView.getContext("2d"), mapInk = document.createElement("canvas");
  let land = null, mapFit = {}, mapArrow = "", mapOpened = 0, sinceMap = 1;

  // The same lines of the manifest app/tile.cpp reads. The roads run on past the tiles, so they are clipped to them when drawn.
  function readLand(text) {
    const tiles = [], names = new Map(), roads = [], crossings = new Map();
    let size = 0;
    for (const line of text.split("\n")) {
      const [word, ...rest] = line.trim().split(/\s+/);
      if (word === "tile_size") size = Number(rest[0]);
      else if (word === "tile") tiles.push([Number(rest[0]), Number(rest[1])]);
      else if (word === "street") names.set(rest[0], rest.slice(1).join(" "));
      else if (word === "road") roads.push(rest.slice(1).map(Number));
      else if (word === "sign") {
        for (const id of rest.slice(2, 4)) {
          if (!crossings.has(id)) crossings.set(id, []);
          crossings.get(id).push([Number(rest[0]), Number(rest[1])]);
        }
      }
    }
    if (!size || !tiles.length) return null;
    const across = tiles.map(([ix]) => ix), up = tiles.map(([, iy]) => iy);
    return {size, tiles, names, roads, crossings, have: new Set(tiles.map(String)),
      west: Math.min(...across) * size, east: (Math.max(...across) + 1) * size,
      south: Math.min(...up) * size, north: (Math.max(...up) + 1) * size};
  }

  const mapPoint = (east, north) => ({x: mapFit.left + (east - land.west) * mapFit.scale, y: mapFit.top + (land.north - north) * mapFit.scale});
  const token = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

  // The names go at crossings, the streets with the most crossings first, and any that would touch another stay off.
  function labelStreets(g) {
    const taken = [], streets = [...land.crossings].filter(([id]) => land.names.get(id)).sort((a, b) => b[1].length - a[1].length);
    const corner = mapPoint(land.west, land.north), end = mapPoint(land.east, land.south);
    const free = (box) => box.x >= corner.x && box.y >= corner.y && box.x + box.w <= end.x && box.y + box.h <= end.y &&
      taken.every((t) => box.x > t.x + t.w || t.x > box.x + box.w || box.y > t.y + t.h || t.y > box.y + box.h);
    g.font = "600 12px system-ui, -apple-system, 'Segoe UI', sans-serif";
    g.textBaseline = "middle";
    g.lineJoin = "round";
    g.lineWidth = 3;
    g.strokeStyle = token("--bg");
    g.fillStyle = token("--text");
    for (const [id, spots] of streets) {
      const name = land.names.get(id), w = g.measureText(name).width + 4, h = 14, placed = [];
      for (const [east, north] of spots) {
        const {x, y} = mapPoint(east, north);
        if (placed.some((p) => Math.hypot(p.x - x, p.y - y) < MAP_LABEL_GAP)) continue;
        const box = [[x + 3, y - h - 2], [x + 3, y + 2], [x - w - 3, y - h - 2], [x - w - 3, y + 2]]
          .map(([bx, by]) => ({x: bx, y: by, w, h})).find(free);
        if (!box) continue;
        taken.push(box);
        placed.push({x, y});
        g.strokeText(name, box.x + 2, box.y + h / 2);
        g.fillText(name, box.x + 2, box.y + h / 2);
      }
    }
  }

  function inkMap(big) {
    const {width, height, dpr} = mapFit, size = land.size * mapFit.scale, g = mapInk.getContext("2d");
    mapInk.width = width * dpr;
    mapInk.height = height * dpr;
    g.scale(dpr, dpr);
    g.beginPath();
    for (const [ix, iy] of land.tiles) {
      const {x, y} = mapPoint(ix * land.size, (iy + 1) * land.size);
      g.rect(x, y, size, size);
    }
    g.fillStyle = token("--btn");
    g.fill();
    g.save();
    g.clip();
    g.beginPath();
    for (const road of land.roads) {
      for (let i = 0; i + 1 < road.length; i += 2) {
        const {x, y} = mapPoint(road[i], road[i + 1]);
        if (i) g.lineTo(x, y); else g.moveTo(x, y);
      }
    }
    g.strokeStyle = token("--dim");
    g.lineWidth = big ? 1.5 : 0.75;
    g.lineJoin = "round";
    g.stroke();
    g.restore();
    g.beginPath();  // the edge of the map, including any bite out of it where a tile is missing
    for (const [ix, iy] of land.tiles) {
      const {x, y} = mapPoint(ix * land.size, (iy + 1) * land.size);
      const edge = (dx, dy, x0, y0, x1, y1) => {
        if (land.have.has(String([ix + dx, iy + dy]))) return;
        g.moveTo(x + x0 * size, y + y0 * size);
        g.lineTo(x + x1 * size, y + y1 * size);
      };
      edge(0, 1, 0, 0, 1, 0);
      edge(0, -1, 0, 1, 1, 1);
      edge(-1, 0, 0, 0, 0, 1);
      edge(1, 0, 1, 0, 1, 1);
    }
    g.strokeStyle = token("--accent");
    g.lineWidth = 1.5;
    g.stroke();
    if (big) labelStreets(g);
  }

  // Asked again at every tick, which catches a turned phone, a browser zoom and the first touch alike. False while hidden.
  function fitMap() {
    const width = mapView.clientWidth, height = mapView.clientHeight, dpr = Math.min(devicePixelRatio || 1, MAP_MAX_DPR);
    if (!land || !width || !height) return false;
    if (width === mapFit.width && height === mapFit.height && dpr === mapFit.dpr) return true;
    const big = mapBig();
    // Large, a screen with room beside the map puts it level with the Map button, and any other puts it below.
    const top = !big ? MAP_INSET : width - height > 200 ? 8 : $("mapBtn").getBoundingClientRect().bottom + 8;
    const bottom = big ? MAP_CREDIT : MAP_INSET, side = big ? 8 : MAP_INSET;
    const roomX = width - side * 2, roomY = height - top - bottom, spanX = land.east - land.west, spanY = land.north - land.south;
    const scale = Math.min(roomX / spanX, roomY / spanY);
    mapFit = {width, height, dpr, scale, left: side + (roomX - spanX * scale) / 2, top: top + (roomY - spanY * scale) / 2, fill: token("--accent"), edge: token("--bg")};
    mapView.width = width * dpr;
    mapView.height = height * dpr;
    inkMap(big);
    mapArrow = "";
    return true;
  }

  function drawMap() {
    if (!fitMap()) return;
    const s = status(), {x, y} = mapPoint(s.east, s.north), {dpr} = mapFit;
    const arrow = `${x.toFixed(1)} ${y.toFixed(1)} ${s.yaw.toFixed(0)}`;
    if (arrow === mapArrow) return;
    mapArrow = arrow;
    const g = mapPen, length = mapBig() ? 13 : 9;
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.clearRect(0, 0, mapView.width, mapView.height);
    g.drawImage(mapInk, 0, 0);
    g.setTransform(dpr, 0, 0, dpr, x * dpr, y * dpr);
    g.rotate(s.yaw * Math.PI / 180);
    g.beginPath();
    g.moveTo(0, -length);
    g.lineTo(length * 0.65, length * 0.8);
    g.lineTo(0, length * 0.4);
    g.lineTo(-length * 0.65, length * 0.8);
    g.closePath();
    g.fillStyle = mapFit.fill;
    g.strokeStyle = mapFit.edge;
    g.lineWidth = 2;
    g.lineJoin = "round";
    g.stroke();
    g.fill();
  }

  function showMap(dt) {
    sinceMap += dt;
    if (sinceMap < MAP_TICK) return;
    sinceMap = 0;
    drawMap();
  }

  const mapBig = () => document.body.classList.contains("map-open");
  function setMapBig(on) {
    document.body.classList.toggle("map-open", on);
    mapView.setAttribute("aria-label", on ? "Large map. Tap a spot to go there" : "Map of where you are. Opens the large map");
    mapOpened = performance.now();
    if (on) {
      held.clear();
      lift.up = lift.down = false;
      lookId = null;
      if (locked()) document.exitPointerLock();  // a map to click needs a cursor
      note(touching() ? "Tap a spot to go there" : "Click a spot to go there. Esc closes this");
    } else {
      canvas.focus();
      grabMouse();
    }
    drawMap();
  }
  function toggleMap() {
    if (mapBig()) { setMapBig(false); return; }
    settings.map = !settings.map;
    applySettings();
    keepSettings();
  }

  // Only a tap that began on the map and stayed put counts. A drag across it is not one, nor is the second half of a
  // double tap on the small map, which lands on the large one before the player has seen it.
  function wireMap() {
    let down = null;
    mapView.addEventListener("pointerdown", (e) => {
      if (e.pointerType === "touch") useTouch();
      down = e.isPrimary ? {x: e.clientX, y: e.clientY} : null;
    });
    mapView.addEventListener("click", (e) => {
      const from = down;
      down = null;
      if (!shown || !land || !from || Math.hypot(e.clientX - from.x, e.clientY - from.y) > MAP_SLOP) return;
      if (helpOpen()) { setHelp(false); return; }  // as a click on the view closes the panel
      if (!mapBig()) { setMapBig(true); return; }
      if (performance.now() - mapOpened < MAP_SETTLE_MS) return;
      const east = land.west + (e.offsetX - mapFit.left) / mapFit.scale, north = land.north - (e.offsetY - mapFit.top) / mapFit.scale;
      if (travel({east, north})) setMapBig(false);
      else note("That is off the map");
    });
  }

  function useTouch() { document.body.classList.add("touch"); }

  function grabMouse() {
    if (touching() || locked() || !canvas.requestPointerLock) return;
    // Refused in a frame without permission: dragging to look still works.
    try { const p = canvas.requestPointerLock(); if (p && p.catch) p.catch(() => {}); } catch (e) { /* not allowed */ }
  }

  function hold(button, name) {
    const set = (on) => (e) => { e.preventDefault(); lift[name] = on; };
    button.addEventListener("pointerdown", set(true));
    for (const ev of ["pointerup", "pointercancel", "pointerleave"]) button.addEventListener(ev, set(false));
  }

  function wireInput() {
    window.addEventListener("keydown", (e) => {
      if (e.ctrlKey || e.metaKey || e.altKey) return;  // the browser's own shortcuts: Ctrl+F is find, not take off
      if (mapBig()) {  // the large map takes the keys, so nothing moves behind it
        if (!e.repeat && (e.code === "Escape" || e.code === "KeyM")) setMapBig(false);
        return;
      }
      if (e.code === "Tab" && helpOpen()) return;  // Tab goes through the panel's controls
      if ($("help").contains(e.target)) {  // these belong to the focused control, not to the walker
        if (e.code === "Space" || e.code === "Enter" || e.code === "NumpadEnter") {
          // A key held since before the panel opened, or held on Reset, must not press it over and over.
          if (e.repeat && !e.target.closest(".setting")) e.preventDefault();
          return;
        }
        if (e.code.startsWith("Arrow")) return;  // scrolls the panel
      }
      if (!e.repeat && e.code === "KeyF") toggleFly();
      if (!e.repeat && e.code === "KeyB") toggleBike();
      if (!e.repeat && e.code === "KeyM") {
        if (e.shiftKey && shown && !helpOpen()) setMapBig(true);  // a locked mouse has no cursor to click the map with
        else toggleMap();
      }
      if (e.code === "PageUp") { changeSpeed(1); e.preventDefault(); }
      if (e.code === "PageDown") { changeSpeed(-1); e.preventDefault(); }
      if (e.code === "Tab") {
        e.preventDefault();
        if (e.repeat) return;
        settings.status = !settings.status;
        applySettings();
        keepSettings();
        if (!settings.status) note("Panel hidden: Tab brings it back");
      }
      if (!e.repeat && e.code === "KeyH") setHelp(!helpOpen());
      if (e.code === "Escape" && helpOpen()) setHelp(false);
      if (!e.repeat && e.code === "KeyR") goToStart();
      if (e.code === "Comma") changeSensitivity(-1);
      if (e.code === "Period") changeSensitivity(1);
      if (e.code === "Digit9") changeFov(-1);
      if (e.code === "Digit0") changeFov(1);
      if (!e.repeat && e.code === "KeyP" && shown) shotWanted = true;
      if (!e.repeat && e.code === "KeyL" && shown) share();
      if (!e.repeat && e.code === "KeyI") {
        settings.invert = !settings.invert;
        note(settings.invert ? "Mouse look inverted" : "Mouse look normal");
        keepSettings();
      }
      if (!e.repeat && e.code === "Backspace") {
        e.preventDefault();
        resetSettings();
      }
      if (e.code === "Minus") changeView(-1);
      if (e.code === "Equal") changeView(1);
      if (!e.repeat && e.code === "BracketLeft") shiftSun(-0.5);
      if (!e.repeat && e.code === "BracketRight") shiftSun(0.5);
      held.add(e.code);
      if (e.code.startsWith("Arrow") || e.code === "Space") e.preventDefault();
    });
    window.addEventListener("keyup", (e) => held.delete(e.code));
    window.addEventListener("blur", () => { held.clear(); lift.up = lift.down = false; });

    canvas.addEventListener("wheel", (e) => {
      e.preventDefault();
      if (e.deltaY !== 0) changeSpeed(e.deltaY < 0 ? 1 : -1);
    }, {passive: false});

    canvas.addEventListener("pointerdown", (e) => {
      if (e.pointerType === "touch") useTouch();
      if (helpOpen()) setHelp(false);
      if (e.pointerType === "mouse") grabMouse();
      if (lookId === null) { lookId = e.pointerId; lookX = e.clientX; lookY = e.clientY; }
    });
    canvas.addEventListener("pointermove", (e) => {
      if (locked()) {
        // The first events after a lock carry where the cursor jumped from, not a hand's movement.
        if (settling > 0) { settling--; return; }
        if (Math.abs(e.movementX) < MAX_MOUSE_STEP && Math.abs(e.movementY) < MAX_MOUSE_STEP) look(e.movementX, e.movementY, settings.sensitivity);
        return;
      }
      if (e.pointerId !== lookId) return;
      look(e.clientX - lookX, e.clientY - lookY, e.pointerType === "touch" ? TOUCH_DEG : settings.sensitivity);
      lookX = e.clientX; lookY = e.clientY;
    });
    document.addEventListener("pointerlockchange", () => {
      settling = 3; lookId = null;
      if (!locked() && shown && !helpOpen() && !mapBig()) note("Click to look around");
    });
    const endLook = (e) => { if (e.pointerId === lookId) lookId = null; };
    canvas.addEventListener("pointerup", endLook);
    canvas.addEventListener("pointercancel", endLook);
    $("map").addEventListener("pointerup", endLook);  // a drag let go over the map must not leave the view following the mouse

    const stick = $("stick"), knob = $("knob");
    let stickId = null;
    const steer = (e) => {
      const r = stick.getBoundingClientRect(), reach = r.width / 2;
      let x = e.clientX - (r.left + reach), y = e.clientY - (r.top + reach);
      const d = Math.hypot(x, y) || 1, limit = Math.min(d, reach);
      x *= limit / d; y *= limit / d;
      knob.style.transform = `translate(${x}px, ${y}px)`;
      move.right = x / reach; move.ahead = -y / reach; move.run = d >= reach * 0.92;
    };
    stick.addEventListener("pointerdown", (e) => {
      e.preventDefault();
      stickId = e.pointerId;
      try { stick.setPointerCapture(stickId); } catch (err) { /* synthetic pointer */ }
      steer(e);
    });
    stick.addEventListener("pointermove", (e) => { if (e.pointerId === stickId) steer(e); });
    const release = (e) => {
      if (e.pointerId !== stickId) return;
      stickId = null;
      move.ahead = move.right = 0; move.run = false;
      knob.style.transform = "";
    };
    stick.addEventListener("pointerup", release);
    stick.addEventListener("pointercancel", release);

    hold($("upBtn"), "up");
    hold($("downBtn"), "down");
    $("jumpBtn").addEventListener("pointerdown", (e) => { e.preventDefault(); if (mod) mod._otw_jump(); });
    $("fasterBtn").addEventListener("click", () => changeSpeed(2));
    $("slowerBtn").addEventListener("click", () => changeSpeed(-2));
    $("flyBtn").addEventListener("click", () => { toggleFly(); canvas.focus(); });
    $("bikeBtn").addEventListener("click", () => { toggleBike(); canvas.focus(); });
    $("mapBtn").addEventListener("click", () => { toggleMap(); if (!helpOpen()) canvas.focus(); });
    $("helpBtn").addEventListener("click", () => setHelp(!helpOpen()));
    $("start").addEventListener("click", () => setHelp(false));
    $("homeBtn").addEventListener("click", () => { goToStart(); setHelp(false); });
    $("shareBtn").addEventListener("click", share);
    const stepper = (name, change, step) => {
      $(name + "Down").addEventListener("click", () => change(-step));
      $(name + "Up").addEventListener("click", () => change(step));
    };
    stepper("speed", changeSpeed, 1);
    stepper("view", changeView, 1);
    stepper("time", shiftSun, 0.5);
    stepper("sensitivity", changeSensitivity, 1);
    $("resetBtn").addEventListener("click", resetSettings);
    // Opening it near the foot of the panel would otherwise leave it out of sight on a short screen.
    for (const panel of [$("places"), $("settings")]) panel.addEventListener("toggle", () => { if (panel.open) panel.scrollIntoView({block: "nearest"}); });
  }

  /* ------------------------------------------------------- frames */
  function frame(t, now, dt) {
    const ahead = move.ahead + keyed("KeyW", "KeyS"), right = move.right + keyed("KeyD", "KeyA");
    const up = (lift.up ? 1 : 0) - (lift.down ? 1 : 0) +
      (held.has("KeyE") || held.has("Space") ? 1 : 0) - (held.has("KeyQ") ? 1 : 0);
    const run = move.run || held.has("ShiftLeft") || held.has("ShiftRight");
    if (!flying && held.has("Space")) mod._otw_jump();
    dyaw += keyed("ArrowRight", "ArrowLeft") * KEY_DEG * dt;
    dpitch += keyed("ArrowUp", "ArrowDown") * KEY_DEG * 0.66 * dt;
    zoom += ((held.has("KeyZ") ? ZOOMED : 1) - zoom) * Math.min(1, dt * 12);
    mod._otw_set_fov(settings.fov * zoom);
    const at = mod._otw_step(dt, ahead, right, up, run ? 1 : 0, dyaw, dpitch);
    dyaw = dpitch = 0;
    if (shown && performance.now() > noteFree) {
      // Looks further than a frame's travel, or fast flight stops short of the edge and never sees it.
      const reach = Math.max(10, (flying ? settings.fly : riding ? settings.ride : settings.walk) * 8 * dt);
      const limit = mod._otw_limit(ahead, right, up, reach);
      if (limit === 1) note("The map ends here");
      if (limit === 2) note("Highest you can fly: 400 m above the ground");
    }
    if (at) {
      const f = mod.HEAPF32, i = at >> 2, c = kit.cam;
      c.yaw = f[i]; c.pitch = f[i + 1]; c.dist = f[i + 2]; c.tx = f[i + 3]; c.ty = f[i + 4]; c.tz = f[i + 5];
      // The kit's camera cannot tilt, so the canvas does. A steady zoom keeps its corners covered without pumping in turns.
      if (riding && !lessMotion.matches) canvas.style.transform = `rotate(${(-f[i + 6]).toFixed(2)}deg) scale(1.07)`;
    }
    pump();
    if (shown) showStatus(dt);
    if (shown) showMap(dt);
    if (!shown) {
      StrataLoader.progress(0.5 + 0.5 * Math.min(tilesIn / 4, 1), `${tilesIn} map tiles`);
      if (status().ready) {
        shown = true;
        StrataLoader.done();
        if (stored.get("old-town-walk-seen") !== "1") setHelp(true);
        else started();
        arriveFromLink();
      }
    }
  }

  function status() { return JSON.parse(mod.UTF8ToString(mod._otw_status())); }

  async function boot() {
    if (coarse) useTouch();
    loadSettings();
    wireInput();
    wireMap();
    mod = await StrataLoader.load({
      script: "walk.js", factory: "createWalk", wasm: "walk.wasm", bytes: __WASM_BYTES__,
      title: "Old Town Walk", poster: "poster.png", fallback: MORE,
    });
    mod.onTileWanted = (ix, iy) => { queue.push([ix, iy]); };
    const answer = await fetch("tiles/manifest.txt");
    if (!answer.ok) throw Object.assign(new Error("The map did not download."), {code: "network"});
    const manifest = await answer.text();
    land = readLand(manifest);
    listPlaces(manifest);
    kit = StrataKit.create({
      module: mod, canvas, idleOrbit: false, dprCap: coarse ? 1.5 : 2,
      limits: {minPitch: -2, maxPitch: 2, minDist: 0.1, maxDist: 2},
      camera: {yaw: 0, pitch: 0, dist: 1, tx: 0, ty: 2, tz: 0},
      frame,
      after: takeShot,
      onSlow: () => StrataLoader.slow(),
      onFatal: (code, message) => StrataLoader.fallback(code, message),
      onRestore: () => note("Reloading the map."),
    });
    kit.setSun(sunTime, PLACE.lat, PLACE.lon);
    const text = mod.stringToNewUTF8(manifest);
    // ?reach=<metres> narrows how far the map loads, as ?quality= sets the tier. The smoke test needs it:
    // a software renderer on a small machine cannot draw the full view and still take input.
    const started = mod._otw_start(text, parseFloat(search.get("reach")) || settings.view);
    mod._free(text);
    if (!started) throw Object.assign(new Error("The map could not be read."), {code: "engine"});
    applySettings();
    window.oldTownWalk = {status, kit, shiftSun, travel, get sunTime() { return sunTime; }};
    window.oldTownWalk.mapPoint = mapPoint;
    window.oldTownWalk.night = () => JSON.parse(mod.UTF8ToString(mod._otw_night()));
    window.oldTownWalk.setLamps = (on) => mod._otw_set_lamps(on ? 1 : 0);
    kit.start();
  }

  boot().catch((e) => {
    console.error(e);
    if (e && e.code && StrataLoader.state === "loading") StrataLoader.fallback(e.code, e.message);
  });
})();
