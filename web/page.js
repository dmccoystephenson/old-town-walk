/* Old Town Walk's page: boots the engine behind Strata's loading shell, downloads the tiles the player
   is near, and turns keys, the mouse and fingers into the walker's input. Stores nothing and reports
   nothing. */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const canvas = $("view");
  const MORE = {href: "https://danielstephenson.dev/play", text: "More games"};
  const PLACE = {lat: 40.5877, lon: -105.0761};  // Old Town, for where the sun stands
  const MIDSUMMER_AFTERNOON = Date.UTC(2026, 6, 15, 21, 0) / 1000;
  const MOUSE_DEG = 0.15, TOUCH_DEG = 0.3, KEY_DEG = 90, MAX_MOUSE_STEP = 300, CONCURRENT = 3, RETRIES = 3;
  const search = new URLSearchParams(location.search);
  const coarse = matchMedia("(pointer: coarse)").matches || search.get("touch") === "1";
  const stored = StrataKit.store;


  const FLY = [5, 800], WALK = [0.7, 14], VIEW = [200, 1500], NOTCH = 1.25, VIEW_STEP = 100;
  const settings = {fly: 20, walk: 1.4, view: coarse ? 500 : 700};
  const clamp = (v, [low, high]) => Math.min(high, Math.max(low, v));

  let mod = null, kit = null, sunTime = MIDSUMMER_AFTERNOON, flying = false, shown = false, failed = false;

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

  function pump() {
    while (downloading < CONCURRENT && queue.length) {
      const [ix, iy] = queue.shift();
      downloading++;
      download(ix, iy).then((ok) => {
        downloading--;
        if (!ok) note("Part of the map did not download.");
        if (!ok && !shown && !failed) { failed = true; StrataLoader.fallback("network", "The map did not download."); }
        pump();
      });
    }
  }

  /* ------------------------------------------------------- input */
  const held = new Set();
  const move = {ahead: 0, right: 0, run: false};  // the stick
  const lift = {up: false, down: false};
  let dyaw = 0, dpitch = 0, lookId = null, lookX = 0, lookY = 0, settling = 0;

  const keyed = (a, b) => (held.has(a) ? 1 : 0) - (held.has(b) ? 1 : 0);
  const locked = () => document.pointerLockElement === canvas;

  function look(dx, dy, deg) { dyaw += dx * deg; dpitch -= dy * deg; }

  function setFlying(on) {
    flying = on;
    document.body.classList.toggle("flying", on);
    $("flyBtn").setAttribute("aria-pressed", String(on));
  }
  function toggleFly() { if (mod) setFlying(mod._otw_toggle_fly() === 1); }

  function shiftSun(hours) {
    sunTime += hours * 3600;
    if (kit) kit.setSun(sunTime, PLACE.lat, PLACE.lon);
  }

  function changeSpeed(notches) {
    const key = flying ? "fly" : "walk", range = flying ? FLY : WALK, before = settings[key];
    settings[key] = clamp(before * Math.pow(NOTCH, notches), range);
    if (settings[key] === before) note(notches > 0 ? "That is the top speed" : "That is the lowest speed");
    if (mod) mod._otw_set_speeds(settings.fly, settings.walk);
  }
  function changeView(steps) {
    const before = settings.view;
    settings.view = clamp((Math.round(before / VIEW_STEP) + steps) * VIEW_STEP, VIEW);
    if (settings.view === before) note(steps > 0 ? "That is the longest view" : "That is the shortest view");
    if (mod) mod._otw_set_radius(settings.view);
  }

  let noteTimer = 0;
  function note(text) {
    const n = $("note");
    n.textContent = text;
    n.style.display = "block";
    clearTimeout(noteTimer);
    noteTimer = setTimeout(() => { n.style.display = "none"; }, 4000);
  }

  function useTouch() { document.body.classList.add("touch"); }

  function grabMouse() {
    if (document.body.classList.contains("touch") || locked() || !canvas.requestPointerLock) return;
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
      if (!e.repeat && e.code === "KeyF") toggleFly();
      if (e.code === "PageUp") { changeSpeed(1); e.preventDefault(); }
      if (e.code === "PageDown") { changeSpeed(-1); e.preventDefault(); }
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
      if (e.pointerType === "mouse") grabMouse();
      if (lookId === null) { lookId = e.pointerId; lookX = e.clientX; lookY = e.clientY; }
    });
    canvas.addEventListener("pointermove", (e) => {
      if (locked()) {
        // The first events after a lock carry where the cursor jumped from, not a hand's movement.
        if (settling > 0) { settling--; return; }
        if (Math.abs(e.movementX) < MAX_MOUSE_STEP && Math.abs(e.movementY) < MAX_MOUSE_STEP) look(e.movementX, e.movementY, MOUSE_DEG);
        return;
      }
      if (e.pointerId !== lookId) return;
      look(e.clientX - lookX, e.clientY - lookY, e.pointerType === "touch" ? TOUCH_DEG : MOUSE_DEG);
      lookX = e.clientX; lookY = e.clientY;
    });
    document.addEventListener("pointerlockchange", () => { settling = 3; lookId = null; });
    const endLook = (e) => { if (e.pointerId === lookId) lookId = null; };
    canvas.addEventListener("pointerup", endLook);
    canvas.addEventListener("pointercancel", endLook);

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
    $("flyBtn").addEventListener("click", () => { toggleFly(); canvas.focus(); });
    $("helpBtn").addEventListener("click", () => $("help").classList.toggle("on"));
    $("start").addEventListener("click", () => {
      $("help").classList.remove("on");
      stored.set("old-town-walk-seen", "1");
      canvas.focus();
      grabMouse();
    });
  }

  /* ------------------------------------------------------- frames */
  function frame(t, now, dt) {
    const ahead = move.ahead + keyed("KeyW", "KeyS"), right = move.right + keyed("KeyD", "KeyA");
    const up = (lift.up ? 1 : 0) - (lift.down ? 1 : 0) +
      (held.has("KeyE") || held.has("Space") ? 1 : 0) - (held.has("KeyQ") || held.has("ControlLeft") ? 1 : 0);
    const run = move.run || held.has("ShiftLeft") || held.has("ShiftRight");
    if (!flying && held.has("Space")) mod._otw_jump();
    dyaw += keyed("ArrowRight", "ArrowLeft") * KEY_DEG * dt;
    dpitch += keyed("ArrowUp", "ArrowDown") * KEY_DEG * 0.66 * dt;
    const at = mod._otw_step(dt, ahead, right, up, run ? 1 : 0, dyaw, dpitch);
    dyaw = dpitch = 0;
    if (at) {
      const f = mod.HEAPF32, i = at >> 2, c = kit.cam;
      c.yaw = f[i]; c.pitch = f[i + 1]; c.dist = f[i + 2]; c.tx = f[i + 3]; c.ty = f[i + 4]; c.tz = f[i + 5];
    }
    pump();
    if (!shown) {
      StrataLoader.progress(0.5 + 0.5 * Math.min(tilesIn / 4, 1), `${tilesIn} map tiles`);
      if (status().ready) {
        shown = true;
        StrataLoader.done();
        if (stored.get("old-town-walk-seen") !== "1") $("help").classList.add("on");
      }
    }
  }

  function status() { return JSON.parse(mod.UTF8ToString(mod._otw_status())); }

  async function boot() {
    if (coarse) useTouch();
    wireInput();
    mod = await StrataLoader.load({
      script: "walk.js", factory: "createWalk", wasm: "walk.wasm", bytes: __WASM_BYTES__,
      title: "Old Town Walk", poster: "poster.png", fallback: MORE,
    });
    mod.onTileWanted = (ix, iy) => { queue.push([ix, iy]); };
    const answer = await fetch("tiles/manifest.txt");
    if (!answer.ok) throw Object.assign(new Error("The map did not download."), {code: "network"});
    const manifest = await answer.text();
    kit = StrataKit.create({
      module: mod, canvas, idleOrbit: false, dprCap: coarse ? 1.5 : 2,
      limits: {minPitch: -2, maxPitch: 2, minDist: 0.1, maxDist: 2},
      camera: {yaw: 0, pitch: 0, dist: 1, tx: 0, ty: 2, tz: 0},
      frame,
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
    window.oldTownWalk = {status, kit, shiftSun, get sunTime() { return sunTime; }};
    kit.start();
  }

  boot().catch((e) => {
    console.error(e);
    if (e && e.code && StrataLoader.state === "loading") StrataLoader.fallback(e.code, e.message);
  });
})();
