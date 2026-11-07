// Smoke test for the browser build in headless Chromium (Playwright).
//
//   node web/smoke/smoke.cjs <url> <out-dir> [--phone]
//
// Plays the way a person does, with real keys, mouse moves and (on the phone profile) touch events
// through the browser's input layer: walks, runs into nothing silly, looks around, takes off, climbs,
// lands, and loses and gets back the graphics context. Fails on any page error, on a frame that is
// flat, and on a tile that fails to download. On the phone, every control is on screen, at least 44
// CSS px, and none overlaps another or the map, upright or on its side. Writes PNGs to <out-dir>.
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { chromium, devices } = require('playwright');

const [url, outDir] = process.argv.slice(2);
const phone = process.argv.includes('--phone');
if (!url || !outDir) {
  console.error('usage: smoke.cjs <url> <out-dir> [--phone]');
  process.exit(2);
}
fs.mkdirSync(outDir, { recursive: true });

// Minimal PNG decoder (8-bit RGB/RGBA, non-interlaced: what Chromium writes).
function decodePng(buf) {
  let pos = 8, width = 0, height = 0, channels = 0;
  const idat = [];
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString('ascii', pos + 4, pos + 8);
    const data = buf.subarray(pos + 8, pos + 8 + len);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      channels = { 2: 3, 6: 4 }[data[9]];
      if (data[8] !== 8 || !channels || data[12] !== 0) throw new Error('unsupported PNG');
    } else if (type === 'IDAT') idat.push(data);
    pos += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = width * channels, out = Buffer.alloc(stride * height);
  for (let y = 0; y < height; y++) {
    const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    const filter = raw[y * (stride + 1)];
    for (let x = 0; x < stride; x++) {
      const a = x >= channels ? out[y * stride + x - channels] : 0;
      const b = y ? out[(y - 1) * stride + x] : 0;
      const c = x >= channels && y ? out[(y - 1) * stride + x - channels] : 0;
      let p = 0;
      if (filter === 1) p = a;
      else if (filter === 2) p = b;
      else if (filter === 3) p = (a + b) >> 1;
      else if (filter === 4) {
        const q = a + b - c, pa = Math.abs(q - a), pb = Math.abs(q - b), pc = Math.abs(q - c);
        p = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      out[y * stride + x] = (line[x] + p) & 255;
    }
  }
  return { width, height, channels, data: out };
}

// The share of two same-sized pictures that differs by more than a little.
function changed(fileA, fileB) {
  const a = decodePng(fs.readFileSync(fileA)), b = decodePng(fs.readFileSync(fileB));
  let moved = 0, seen = 0;
  for (let i = 0; i < a.data.length; i += a.channels * 7, seen++) {
    if (Math.abs(a.data[i] - b.data[i]) + Math.abs(a.data[i + 1] - b.data[i + 1]) + Math.abs(a.data[i + 2] - b.data[i + 2]) > 30) moved++;
  }
  return moved / seen;
}

// Distinct coarse colours in a screenshot: a flat frame has a handful.
function colours(file) {
  const img = decodePng(fs.readFileSync(file));
  const seen = new Set();
  for (let i = 0; i < img.data.length; i += img.channels * 7) {
    seen.add((img.data[i] >> 3) << 10 | (img.data[i + 1] >> 3) << 5 | (img.data[i + 2] >> 3));
  }
  return seen.size;
}

const check = (ok, what) => {
  if (!ok) throw new Error('FAILED: ' + what);
  console.log('ok: ' + what);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const SOLID = { east: 493580, north: 4492960 };  // inside a building, from the pinned map

(async () => {
  const browser = await chromium.launch({
    args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
  });
  const context = await browser.newContext(
    phone ? { ...devices['iPhone 13'], hasTouch: true } : { viewport: { width: 1280, height: 720 } });
  const page = await context.newPage();
  const errors = [], requests = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('request', (r) => requests.push(r.url()));
  page.on('response', (r) => { if (r.status() >= 400 && !r.url().endsWith('favicon.ico')) errors.push(`${r.status()} ${r.url()}`); });

  // Headless Chromium has no display to hold the page back, so it queues frames faster than a software
  // renderer draws them and the page stops answering: each frame here waits for the GPU process, then
  // leaves a gap for input to get through.
  // Pictures come straight off the canvas, in the task that drew them. page.screenshot goes through
  // the compositor, which falls tens of seconds behind a scene this size, so it times out; this reads
  // what GL drew, without the page's controls on top.
  await context.addInitScript(() => {
    const getContext = HTMLCanvasElement.prototype.getContext;
    let gl = null;
    HTMLCanvasElement.prototype.getContext = function (kind, ...rest) {
      const made = getContext.call(this, kind, ...rest);
      if (kind === 'webgl2' && made) gl = made;
      return made;
    };
    const raf = window.requestAnimationFrame.bind(window);
    window.requestAnimationFrame = (draw) => setTimeout(() => raf((now) => {
      draw(now);
      if (gl) gl.finish();
      if (window.takeFrame) {
        window.takeFrame(document.getElementById('view').toDataURL('image/png'));
        window.takeFrame = null;
      }
    }), 100);
  });
  await page.goto(url);
  const status = () => page.evaluate(() => window.oldTownWalk.status());
  const shoot = async (file) => {
    const png = await page.evaluate(() => new Promise((done) => { window.takeFrame = done; }));
    fs.writeFileSync(file, Buffer.from(png.split(',')[1], 'base64'));
  };
  await page.waitForFunction(() => window.oldTownWalk && window.oldTownWalk.status().ready === 1, null, { timeout: 180000 });
  const first = await status();
  check(first.yaw === 225, 'the walk begins facing down Old Town Square');
  console.log('ready', JSON.stringify(first));
  await page.waitForFunction(() => window.oldTownWalk.status().street !== '', null, { timeout: 30000 });
  const named = await status();
  check(named.street.length > 0, `the status names the street (${named.street})`);
  check(named.signs > 0, `street signs stand nearby (${named.signs})`);
  check(requests.filter((r) => /\.tile\.gz$/.test(r)).length >= 4, 'tiles arrive on demand');
  check(requests.every((r) => r.startsWith(new URL(url).origin) || r.startsWith('data:') || r.startsWith('blob:')), 'nothing is fetched from another site');
  await page.waitForFunction(() => document.querySelector('.sk-loader') === null ||
    getComputedStyle(document.querySelector('.sk-loader')).display === 'none' ||
    getComputedStyle(document.querySelector('.sk-loader')).opacity === '0', null, { timeout: 30000 });

  const help = page.locator('#help');
  check(await help.isVisible(), 'the controls show without a keyboard');
  check(!(await page.locator('#settings').isVisible()), 'the welcome leaves Settings out of the way');
  await page.locator('#start').click();
  check(!(await help.isVisible()), 'Start closes them');
  const noted = (text) => page.waitForFunction((t) => {
    const n = document.getElementById('note');
    return n.style.display === 'block' && n.textContent.includes(t);
  }, text, { timeout: 30000 });
  await sleep(1500);
  const groundShot = path.join(outDir, 'ground.png');
  await shoot(groundShot);
  check(colours(groundShot) > 40, 'the street is drawn');
  const shows = (id) => page.locator(id).textContent();
  const onBody = (name) => page.evaluate((n) => document.body.classList.contains(n), name);
  const mapShown = () => page.locator('#map').isVisible();
  const savedMap = () => page.evaluate(() => JSON.parse(localStorage.getItem('old-town-walk-settings') || '{}').map);
  let tapAt = (x, y) => page.mouse.click(x, y);
  check(await mapShown(), 'the map shows');
  await page.waitForFunction(() => {  // the arrow is where the player is: the pixel under the spot is the accent colour
    const c = document.getElementById('map'), s = window.oldTownWalk.status(), p = window.oldTownWalk.mapPoint(s.east, s.north), k = c.width / c.clientWidth;
    const [r, g, b, a] = c.getContext('2d').getImageData(Math.round(p.x * k), Math.round(p.y * k), 1, 1).data;
    return r > 230 && g > 170 && g < 200 && b < 90 && a > 250;
  }, null, { timeout: 90000 });
  check(true, 'the map is drawn with its arrow');
  // Every box that matters, on foot and in the air, pairwise: the map against all of them and the controls against each other.
  const overlaps = () => page.evaluate(() => {
    const find = () => ['#brand', '#bikeBtn', '#flyBtn', '#mapBtn', '#helpBtn', '#status', '#stick', '#height', '#speed', '#jumpBtn', '#map']
      .map((sel) => [sel, document.querySelector(sel).getBoundingClientRect()]).filter(([, b]) => b.width > 0);
    const clashes = (boxes) => boxes.flatMap(([a, p], i) => boxes.slice(i + 1).filter(([, q]) =>
      p.left < q.right && q.left < p.right && p.top < q.bottom && q.top < p.bottom).map(([b]) => `${a} on ${b}`));
    const flying = document.body.classList.contains('flying'), out = [];
    for (const on of [false, true]) {
      document.body.classList.toggle('flying', on);
      const boxes = find();
      out.push(...clashes(boxes));
      for (const [sel, b] of boxes) if (b.left < 0 || b.top < 0 || b.right > innerWidth || b.bottom > innerHeight) out.push(`${sel} off screen`);
    }
    document.body.classList.toggle('flying', flying);
    return out;
  });
  const reset = page.locator('#resetBtn');
  const level = await status();
  check(Math.abs(level.pitch) < 15, `starting does not swing the view (pitch ${level.pitch})`);

  if (phone) {
    const cdp = await context.newCDPSession(page);
    const touch = (type, points) => cdp.send('Input.dispatchTouchEvent', { type, touchPoints: points });
    const middle = async (sel) => {
      const b = await page.locator(sel).boundingBox();
      return { x: b.x + b.width / 2, y: b.y + b.height / 2 };
    };
    for (const sel of ['#flyBtn', '#mapBtn', '#helpBtn', '#stick', '#jumpBtn']) {
      const b = await page.locator(sel).boundingBox();
      check(b && b.width >= 44 && b.height >= 44 && b.x >= 0 && b.y >= 0 &&
        b.x + b.width <= 390 && b.y + b.height <= page.viewportSize().height, `${sel} is on screen and big enough`);
    }
    tapAt = async (x, y) => {
      await touch('touchStart', [{ x, y, id: 6 }]);
      await touch('touchEnd', []);
      await sleep(250);
    };
    // The map stays clear of everything on the phones it has to fit, upright and on its side.
    const profile = page.viewportSize();
    for (const [width, height] of [[390, 844], [320, 568], [844, 390], [568, 320], [profile.width, profile.height]]) {
      await page.setViewportSize({ width, height });
      const clash = await overlaps();
      check(clash.length === 0 && await mapShown(), `at ${width}x${height} nothing overlaps${clash.length ? ': ' + clash.join(', ') : ''}`);
    }
    await tapAt(...Object.values(await middle('#mapBtn')));
    check(!(await mapShown()) && await savedMap() === false, 'the Map button hides the map, and that is kept');
    await tapAt(...Object.values(await middle('#mapBtn')));
    check(await mapShown() && await savedMap() === true, 'and brings it back');
    // Walk with the stick while a second finger looks.
    const c = await middle('#stick');
    const before = await status();
    await touch('touchStart', [{ x: c.x, y: c.y, id: 1 }]);
    await touch('touchMove', [{ x: c.x, y: c.y - 40, id: 1 }]);
    await touch('touchStart', [{ x: c.x, y: c.y - 40, id: 1 }, { x: 300, y: 300, id: 2 }]);
    for (let i = 1; i <= 6; i++) {
      await touch('touchMove', [{ x: c.x, y: c.y - 40, id: 1 }, { x: 300 - i * 6, y: 300, id: 2 }]);
      await sleep(120);
    }
    await sleep(2500);
    await touch('touchEnd', []);
    const after = await status();
    check(Math.hypot(after.x - before.x, after.z - before.z) > 0.5, `the stick walks (${Math.hypot(after.x - before.x, after.z - before.z).toFixed(1)} m)`);
    check(Math.abs(after.yaw - before.yaw) > 3, 'a second finger looks at the same time');
    // Take off, climb, come down.
    const tap = async (sel) => {
      const p = await middle(sel);
      await touch('touchStart', [{ x: p.x, y: p.y, id: 3 }]);
      await touch('touchEnd', []);
      await sleep(250);
    };
    const leap = await middle('#jumpBtn');
    await touch('touchStart', [{ x: leap.x, y: leap.y, id: 5 }]);
    await touch('touchEnd', []);
    await page.waitForFunction(() => window.oldTownWalk.status().height > 2, null, { timeout: 30000 });
    check(true, 'the Jump button jumps');
    await tap('#bikeBtn');
    await noted('push the stick');
    check(true, 'a phone is told how to ride with the stick');
    const panelBox = await page.locator('#status').boundingBox(), noteBox = await page.locator('#note').boundingBox();
    check(noteBox.y >= panelBox.y + panelBox.height, 'the message clears the status panel');
    await tap('#bikeBtn');
    await tap('#flyBtn');
    check((await status()).flying === 1, 'Fly takes off');
    await noted('climbs');
    check(true, 'taking off says how to climb');
    for (const sel of ['#fasterBtn', '#slowerBtn']) {
      const b = await page.locator(sel).boundingBox();
      check(b && b.width >= 44 && b.height >= 44 && b.x >= 0 && b.x + b.width <= 390, `${sel} is on screen and big enough`);
    }
    await page.locator('#fasterBtn').click();
    await page.waitForFunction(() => document.getElementById('status').textContent.includes('31 m/s'), null, { timeout: 30000 });
    check(true, 'the + button flies faster');
    const low = await status();
    const up = await middle('#upBtn');
    await touch('touchStart', [{ x: up.x, y: up.y, id: 4 }]);
    await sleep(2500);
    await touch('touchEnd', []);
    const high = await status();
    check(high.y > low.y + 5, `the climb button climbs (${(high.y - low.y).toFixed(1)} m)`);
    const air = path.join(outDir, 'air.png');
    await shoot(air);
    check(colours(air) > 25, 'the view from the air is drawn');
    await tap('#flyBtn');
    check((await status()).flying === 0, 'Fly lands');
    await tap('#helpBtn');
    check(await page.locator('#homeBtn').isVisible(), 'the controls offer a way back to the start');
    // Settings by finger: every control is big enough, inside the panel, and answers a tap.
    const fingerTap = async (sel) => {
      await page.locator(sel).scrollIntoViewIfNeeded();
      await tap(sel);
    };
    await fingerTap('#settings summary');
    check(await page.locator('#viewUp').isVisible(), 'a tap opens Settings');
    check(!(await page.locator('#sensitivityRow').isVisible()), 'a touch screen is not offered mouse sensitivity');
    const edges = await page.evaluate(() => {
      const panel = document.getElementById('help'), box = panel.getBoundingClientRect();
      return {
        sideways: panel.scrollWidth > panel.clientWidth || document.documentElement.scrollWidth > innerWidth,
        tall: box.height > innerHeight * 0.9 + 1,
        controls: [...panel.querySelectorAll('#settings summary, #settings button')].filter((c) => c.offsetParent).map((c) => {
          const b = c.getBoundingClientRect();
          return {height: b.height, inside: b.left >= box.left && b.right <= box.right};
        }),
      };
    });
    check(!edges.sideways && !edges.tall, 'the panel fits the screen and scrolls only up and down');
    check(edges.controls.every((c) => c.height >= 44 && c.inside), `all ${edges.controls.length} settings controls are at least 44 px and inside the panel`);
    const walking = await shows('#speedValue'), reach = await shows('#viewValue'), hour = await shows('#timeValue');
    await fingerTap('#speedUp');
    await fingerTap('#viewDown');
    await fingerTap('#timeUp');
    check(await shows('#speedValue') !== walking, `the speed stepper changes the speed (${await shows('#speedValue')})`);
    check(await shows('#viewValue') !== reach, `the view stepper changes the view (${await shows('#viewValue')})`);
    check(await shows('#timeValue') !== hour, `the time stepper moves the sun (${await shows('#timeValue')})`);
    const changed = await shows('#viewValue');
    await fingerTap('#resetBtn');
    check(await shows('#resetBtn') === 'Undo reset' && await shows('#viewValue') === reach, 'Reset goes to the defaults and offers to undo');
    await fingerTap('#resetBtn');
    check(await shows('#viewValue') === changed, 'tapping again undoes it');
    await page.locator('#homeBtn').click();
    await noted('Back at the start');
    check(!(await help.isVisible()), 'and it closes them');
  } else {
    const before = await status();
    await page.keyboard.down('KeyW');
    await page.waitForFunction(({ x, z }) => {
      const s = window.oldTownWalk.status();
      return Math.hypot(s.x - x, s.z - z) > 0.6;
    }, { x: before.x, z: before.z }, { timeout: 60000 });
    await page.keyboard.up('KeyW');
    const walked = await status();
    const dist = Math.hypot(walked.x - before.x, walked.z - before.z);
    check(dist > 0.5, `W walks (${dist.toFixed(1)} m)`);
    // Under pointer lock, Playwright's mouse reports absolute jumps that a real one never does, so
    // the locked path gets a known movement, and real mouse input goes through drag to look.
    check(await page.evaluate(() => document.pointerLockElement === document.getElementById('view')), 'clicking takes the mouse');
    const clash = await overlaps();
    check(clash.length === 0, `nothing overlaps${clash.length ? ': ' + clash.join(', ') : ''}`);
    await page.keyboard.press('KeyM');
    check(!(await mapShown()) && await savedMap() === false, 'M hides the map, and that is kept');
    await page.keyboard.press('KeyM');
    check(await mapShown() && await savedMap() === true, 'and M brings it back');
    await page.keyboard.press('Shift+KeyM');
    check(await onBody('map-open'), 'Shift+M opens the large map');
    await page.waitForFunction(() => document.pointerLockElement === null, null, { timeout: 10000 });
    check(true, 'the large map frees the mouse');
    await page.keyboard.press('KeyM');
    check(!(await onBody('map-open')) && await mapShown(), 'M closes it and the small map stays');
    await page.waitForFunction(() => document.pointerLockElement === document.getElementById('view'), null, { timeout: 10000 });
    check(true, 'and the mouse goes back');
    await page.evaluate(() => {
      const view = document.getElementById('view');
      for (let i = 0; i < 6; i++) view.dispatchEvent(new PointerEvent('pointermove', { movementX: 40, movementY: 0, bubbles: true }));
    });
    await page.waitForFunction((yaw) => window.oldTownWalk.status().yaw !== yaw, before.yaw, { timeout: 30000 });
    const lockedLook = await status();
    check(Math.abs(lockedLook.yaw - before.yaw) > 5, `a locked mouse looks (${(lockedLook.yaw - before.yaw).toFixed(1)} degrees)`);
    await page.evaluate(() => {
      const view = document.getElementById('view');
      view.requestPointerLock = () => {};
      document.exitPointerLock();
    });
    await sleep(500);
    await page.mouse.move(640, 360);
    await page.mouse.down();
    await page.mouse.move(760, 360, { steps: 8 });
    await page.mouse.up();
    await sleep(800);
    const turned = await status();
    check(turned.yaw !== lockedLook.yaw, 'dragging the mouse looks');
    check(Math.abs(turned.pitch) < 25, `a sideways move stays level (pitch ${turned.pitch})`);
    await page.keyboard.press('KeyF');
    await sleep(300);
    check((await status()).flying === 1, 'F takes off');
    const low = await status();
    await page.keyboard.down('KeyE');
    await sleep(2500);
    await page.keyboard.up('KeyE');
    const high = await status();
    check(high.y > low.y + 5, `E climbs (${(high.y - low.y).toFixed(1)} m)`);
    await page.keyboard.down('ArrowDown');
    await sleep(1500);
    await page.keyboard.up('ArrowDown');
    await sleep(600);
    const tilted = await status();
    check(tilted.pitch < -5 && tilted.pitch > -85, `the arrow keys tilt the view (pitch ${tilted.pitch})`);
    const air = path.join(outDir, 'air.png');
    await shoot(air);
    check(colours(air) > 25, 'the view from the air is drawn');
    await page.keyboard.press('KeyF');
    await sleep(300);
    check((await status()).flying === 0, 'F lands');
    // The bike goes where its wheels point: it builds up speed, turns in an arc and keeps rolling.
    await page.keyboard.press('KeyB');
    await page.waitForFunction(() => window.oldTownWalk.status().riding === 1, null, { timeout: 5000 });
    check(true, 'B gets on the bike');
    await page.keyboard.down('KeyW');
    await page.waitForFunction(() => window.oldTownWalk.status().speed > 1, null, { timeout: 60000 });
    check(true, 'W pedals');
    const straight = await status();
    await page.keyboard.down('KeyD');
    await page.waitForFunction((yaw) => Math.abs(window.oldTownWalk.status().yaw - yaw) > 5, straight.yaw, { timeout: 60000 });
    await page.keyboard.up('KeyD');
    await page.keyboard.up('KeyW');
    await shoot(path.join(outDir, 'bike.png'));
    check(true, 'D steers the bike');
    // Asked for less motion mid-ride, the lean goes at once and the steering stays. The first wait shows it was there to go.
    const roll = () => new DOMMatrix(getComputedStyle(document.getElementById('view')).transform).b;
    await page.keyboard.down('KeyW');
    await page.keyboard.down('KeyD');
    await page.waitForFunction(`(${roll})() !== 0`, null, { timeout: 60000 });
    check(true, 'the bike leans the canvas');
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await page.waitForFunction(() => getComputedStyle(document.getElementById('view')).transform === 'none', null, { timeout: 30000 });
    const level = await status();
    await page.waitForFunction((yaw) => Math.abs(window.oldTownWalk.status().yaw - yaw) > 5, level.yaw, { timeout: 60000 });
    check((await status()).riding === 1, 'the bike still steers with reduced motion');
    check(await page.evaluate(() => getComputedStyle(document.getElementById('view')).transform === 'none'), 'reduced motion keeps the canvas level through a turn');
    await page.emulateMedia({ reducedMotion: 'no-preference' });
    await page.waitForFunction(`(${roll})() !== 0`, null, { timeout: 30000 });
    check(true, 'the lean comes back with the preference');
    await page.keyboard.up('KeyD');
    await page.keyboard.up('KeyW');
    await page.keyboard.press('PageUp');
    await noted('Bike top speed 15 m/s');
    check(true, 'a speed change says the new top speed');
    await page.keyboard.press('PageDown');
    await page.keyboard.press('KeyB');
    await page.waitForFunction(() => window.oldTownWalk.status().riding === 0, null, { timeout: 5000 });
    check(true, 'B gets off the bike');
    // Speed and view, then a jump on the ground. Every one of these is a setting the player can see.
    const panel = () => page.locator('#status').textContent();
    await page.mouse.move(640, 360);
    await page.mouse.wheel(0, -100);
    await page.waitForFunction(() => document.getElementById('status').textContent.includes('1.8 m/s'), null, { timeout: 30000 });
    check(true, 'the wheel sets the speed');
    await page.keyboard.press('Equal');
    await noted('View 800 m');
    check(true, 'a view change says the new reach');
    await page.keyboard.press('Tab');
    await noted('Tab brings it back');
    await page.keyboard.press('Tab');
    check(true, 'hiding the panel says how to bring it back');
    await page.keyboard.press('Backspace');
    await noted('again to undo');
    await page.keyboard.press('Backspace');
    await noted('Your settings are back');
    check(JSON.parse(await page.evaluate(() => localStorage.getItem('old-town-walk-settings'))).view === 800, 'a second Backspace undoes the first');
    await page.keyboard.down('Shift');
    await page.keyboard.down('KeyW');
    await page.waitForFunction(() => document.getElementById('status').textContent.includes('running'), null, { timeout: 30000 });
    await page.keyboard.up('KeyW');
    await page.keyboard.up('Shift');
    check(true, 'Shift shows on the status panel');
    await page.keyboard.down('Control');
    await page.keyboard.press('KeyF');
    await page.keyboard.up('Control');
    check((await status()).flying === 0, 'Ctrl+F is left to the browser');
    check(JSON.parse(await page.evaluate(() => localStorage.getItem('old-town-walk-settings') || '{}')).view > 0, 'settings are kept in the browser');
    await page.waitForFunction(() => document.getElementById('status').textContent.includes('tiles'), null, { timeout: 30000 });
    check((await panel()).includes('fps'), 'the status panel shows the frame rate');
    await page.keyboard.down('Space');
    await page.waitForFunction(() => window.oldTownWalk.status().height > 2, null, { timeout: 30000 });
    await page.keyboard.up('Space');
    check(true, 'Space jumps');
    await page.keyboard.press('KeyH');
    check(await help.isVisible(), 'H shows the controls');
    await page.keyboard.press('KeyH');
    check(!(await help.isVisible()), 'H hides them');
    await page.keyboard.press('KeyH');
    await page.keyboard.press('Escape');
    check(!(await help.isVisible()), 'Esc hides them too');
    // Settings with the mouse and keys: keys show in the panel, and a focused control keeps its keys.
    await page.keyboard.press('KeyH');
    await page.locator('#settings summary').click();
    check(await page.locator('#sensitivityRow').isVisible(), 'a mouse is offered sensitivity');
    const reach = parseInt(await shows('#viewValue'));
    await page.keyboard.press('Equal');
    check(parseInt(await shows('#viewValue')) === reach + 100, 'a key changes what the panel shows');
    await page.keyboard.press('KeyF');
    check(await shows('#speedName') === 'Flying speed', 'the panel follows the mode');
    await page.keyboard.press('KeyF');
    const hour = await shows('#timeValue');
    await page.keyboard.press('BracketRight');
    check(await shows('#timeValue') !== hour, '] shows in the panel');
    await page.keyboard.press('BracketLeft');
    await sleep(1000);
    const steady = await status();
    await page.locator('#viewUp').click();
    check(parseInt(await shows('#viewValue')) === reach + 200, 'a click steps the view');
    await page.keyboard.press('Enter');
    check(parseInt(await shows('#viewValue')) === reach + 300, 'Enter on a focused stepper steps it once');
    await page.keyboard.press('Space');
    check(parseInt(await shows('#viewValue')) === reach + 400, 'Space on a focused stepper steps it once');
    await page.keyboard.down('ArrowLeft');
    await sleep(800);
    await page.keyboard.up('ArrowLeft');
    await sleep(400);
    const still = await status();
    check(still.yaw === steady.yaw && Math.abs(still.height - steady.height) < 0.3, 'keys on a focused stepper neither turn the view nor jump');
    await page.keyboard.press('Tab');
    check(await page.evaluate(() => document.activeElement.id === 'resetBtn' || document.activeElement.id === 'timeDown'), 'Tab moves to the next control');
    check(!(await page.evaluate(() => document.body.classList.contains('no-status'))), 'and leaves the status panel alone');
    await reset.focus();
    await page.keyboard.down('Enter');
    await page.keyboard.down('Enter');  // the rest come through as repeats
    await page.keyboard.down('Enter');
    await page.keyboard.up('Enter');
    check(await shows('#resetBtn') === 'Undo reset', 'holding Enter on Reset presses it once');
    await reset.click();
    check(parseInt(await shows('#viewValue')) === reach + 400, 'clicking Undo reset brings the settings back');
    await page.keyboard.press('Backspace');
    check(await shows('#resetBtn') === 'Undo reset', 'Backspace and the button are the same reset');
    await page.waitForFunction(() => document.getElementById('resetBtn').textContent === 'Reset to defaults', null, { timeout: 30000 });
    check(true, 'the offer to undo goes away');
    await page.keyboard.press('Escape');
    check(await page.evaluate(() => document.activeElement === document.getElementById('view')), 'closing the panel gives the keys back to the map');
    await page.keyboard.press('KeyR');
    await page.waitForFunction(() => document.getElementById('note').textContent.includes('start'), null, { timeout: 30000 });
    check(true, 'R goes back to the start');
    const home = await status();
    check(Math.hypot(home.x - first.x, home.z - first.z) < 0.5 && home.yaw === first.yaw, 'R returns to the spot the walk began');
    const sun = await page.evaluate(() => window.oldTownWalk.sunTime);
    await page.keyboard.press('BracketRight');
    check((await page.evaluate(() => window.oldTownWalk.sunTime)) > sun, '] moves the sun');
    // Zoom, field of view and the saved picture, from the start, where there is plenty in the frame.
    check((await status()).fov === 70, 'the field of view starts at 70');
    const drawn = () => page.evaluate(() => window.oldTownWalk.kit.stats().frames);
    const drag = async () => {  // two frames later, every step of the drag has reached the walker
      const from = (await status()).yaw;
      await page.mouse.move(640, 360);
      await page.mouse.down();
      await page.mouse.move(760, 360, { steps: 8 });
      await page.mouse.up();
      await page.waitForFunction((f) => window.oldTownWalk.kit.stats().frames >= f, (await drawn()) + 2, { timeout: 30000 });
      return Math.abs((((await status()).yaw - from + 540) % 360) - 180);
    };
    const wideTurn = await drag();
    const wideShot = path.join(outDir, 'wide.png'), zoomShot = path.join(outDir, 'zoomed.png');
    await shoot(wideShot);
    await page.keyboard.down('KeyZ');
    await page.waitForFunction(() => window.oldTownWalk.status().fov < 21.5, null, { timeout: 30000 });
    await shoot(zoomShot);
    const zoomTurn = await drag();
    await page.keyboard.up('KeyZ');
    check(changed(wideShot, zoomShot) > 0.2, 'holding Z closes in on the picture');
    check(zoomTurn > wideTurn * 0.2 && zoomTurn < wideTurn * 0.45, `zoomed, the mouse turns less (${zoomTurn.toFixed(1)} against ${wideTurn.toFixed(1)} degrees)`);
    await page.waitForFunction(() => window.oldTownWalk.status().fov > 69.5, null, { timeout: 30000 });
    check(true, 'letting go of Z goes back to the full view');
    await page.keyboard.press('Digit0');
    await noted('Field of view 75');
    await page.waitForFunction(() => window.oldTownWalk.status().fov === 75, null, { timeout: 30000 });
    check(true, '0 widens the view');
    check(JSON.parse(await page.evaluate(() => localStorage.getItem('old-town-walk-settings'))).fov === 75, 'the field of view is kept with the settings');
    await page.keyboard.press('Backspace');
    await noted('again to undo');
    await page.waitForFunction(() => window.oldTownWalk.status().fov === 70, null, { timeout: 30000 });
    await page.keyboard.press('Backspace');
    await noted('Your settings are back');
    await page.waitForFunction(() => window.oldTownWalk.status().fov === 75, null, { timeout: 30000 });
    check(true, 'Backspace resets the field of view and a second one undoes it');
    await page.keyboard.press('Digit9');
    await noted('Field of view 70');
    for (let i = 0; i < 7; i++) await page.keyboard.press('Digit9');  // six reach 40, and the seventh finds the end
    await noted('That is the narrowest view');
    await page.waitForFunction(() => window.oldTownWalk.status().fov === 40, null, { timeout: 30000 });
    check(true, '9 stops at the desktop\'s narrowest');
    for (let i = 0; i < 6; i++) await page.keyboard.press('Digit0');
    await noted('Field of view 70');
    const [saved] = await Promise.all([page.waitForEvent('download', { timeout: 60000 }), page.keyboard.press('KeyP')]);
    check(saved.suggestedFilename() === 'old-town-walk-000.png', 'P saves a numbered PNG');
    await noted('Saved old-town-walk-000.png');
    const savedFile = path.join(outDir, 'saved.png');
    await saved.saveAs(savedFile);
    const savedSize = decodePng(fs.readFileSync(savedFile)), viewSize = await page.evaluate(() => [document.getElementById('view').width, document.getElementById('view').height]);
    check(savedSize.width === viewSize[0] && savedSize.height === viewSize[1], `the picture is the game view alone (${savedSize.width}x${savedSize.height})`);
    check(colours(savedFile) > 25, 'the picture is not blank');
  }

  // Travel sets the player down clear of any building, and refuses a spot off the map.
  const here = await status();
  const went = await page.evaluate(([e, n]) => window.oldTownWalk.travel({east: e + 300, north: n - 200, yaw: 90}), [here.east, here.north]);
  check(went, 'travel goes to a spot on the map');
  await page.waitForFunction(() => window.oldTownWalk.status().arriving === 0, null, { timeout: 120000 });
  const there = await status();
  check(Math.hypot(there.east - here.east - 300, there.north - here.north + 200) < 90 && there.yaw === 90, 'and sets the player down there');
  check(!(await page.evaluate(([e, n]) => window.oldTownWalk.travel({east: e + 90000, north: n}), [here.east, here.north])), 'travel refuses a spot off the map');

  // The large map: opening and closing it moves nothing, a tap off the map stays put, and a tap on it goes there.
  const openMap = async () => {
    const b = await page.locator('#map').boundingBox();
    await tapAt(b.x + b.width / 2, b.y + b.height / 2);
    await page.waitForFunction(() => document.body.classList.contains('map-open'), null, { timeout: 10000 });
    await sleep(500);  // past the guard against a double tap
  };
  await openMap();
  const fitted = await page.evaluate(() => {
    const c = document.getElementById('map').getBoundingClientRect();
    return { width: c.width, height: c.height, screen: [innerWidth, innerHeight] };
  });
  check(fitted.width === fitted.screen[0] && fitted.height === fitted.screen[1], 'the large map fills the screen');
  await page.keyboard.press('Escape');
  check(!(await onBody('map-open')), 'Esc closes the large map');
  await sleep(600);
  const unmoved = await status();
  check(Math.hypot(unmoved.east - there.east, unmoved.north - there.north) < 2 && unmoved.yaw === there.yaw, 'and nothing moved');
  await openMap();
  await page.locator('#mapBtn').click();
  check(!(await onBody('map-open')), 'the Map button closes it too');
  await openMap();
  await tapAt(3, Math.round(fitted.screen[1] / 2));
  await noted('off the map');
  check(await onBody('map-open') && (await status()).arriving === 0, 'a tap off the map goes nowhere and leaves it open');
  const goal = { east: there.east - 300, north: there.north + 200 };
  const spot = await page.evaluate(({ east, north }) => window.oldTownWalk.mapPoint(east, north), goal);
  await tapAt(spot.x, spot.y);
  await page.waitForFunction(() => !document.body.classList.contains('map-open'), null, { timeout: 10000 });
  await page.waitForFunction(() => window.oldTownWalk.status().arriving === 0, null, { timeout: 120000 });
  const mapped = await status(), missed = Math.hypot(mapped.east - goal.east, mapped.north - goal.north);
  check(missed < 90 && mapped.yaw === there.yaw, `a tap on the large map goes there and closes it (${missed.toFixed(1)} m off)`);
  await page.locator('#mapBtn').click();
  check(await savedMap() === false, 'the map can be left off');
  await page.keyboard.press('Backspace');
  await noted('again to undo');
  check(await mapShown(), 'and a reset brings it back');
  await page.keyboard.press('Backspace');
  await noted('Your settings are back');
  check(!(await mapShown()), 'which a second Backspace undoes');
  await page.locator('#mapBtn').click();

  // Back to the start first: where a run ends depends on the machine's frame rate, and a wall or pavement filling the
  // frame has too few colours to tell a real picture from a flat one.
  await page.keyboard.press('KeyR');
  await page.waitForFunction(() => Math.abs(window.oldTownWalk.status().pitch) < 1 && window.oldTownWalk.status().flying === 0, null, { timeout: 60000 });
  await sleep(1000);

  // A lost context is rebuilt, and the map comes back. It reloads what this spot needs, which is what it needed at the
  // start: the count now can be higher, with tiles left over from travelling that nothing asks for again.
  const held = first.tiles;
  check(await page.evaluate(() => window.oldTownWalk.kit.loseContext()), 'the context can be lost');
  await page.waitForFunction(() => window.oldTownWalk.kit.lost === true, null, { timeout: 10000 });
  await page.evaluate(() => window.oldTownWalk.kit.restoreContext());
  await page.waitForFunction((n) => window.oldTownWalk.kit.lost === false && window.oldTownWalk.status().tiles >= n,
    held, { timeout: 120000 });
  check(true, `a lost context comes back with ${held} tiles`);
  await sleep(1000);
  const backShot = path.join(outDir, 'restored.png');
  await shoot(backShot);
  check(colours(backShot) > 25, 'the frame after a restore is drawn');

  // Share from somewhere other than the start, which a link that did nothing would also land on. After the lost context,
  // since tiles loaded out here would still be in the count that check waits for.
  await page.evaluate(([e, n]) => window.oldTownWalk.travel({east: e + 300, north: n - 200, yaw: 90}), [first.east, first.north]);
  await page.waitForFunction(() => window.oldTownWalk.status().arriving === 0, null, { timeout: 120000 });
  const spot = await status();
  // Stand-ins for the clipboard and the share sheet, so that a refusal can be tested too.
  await page.evaluate(() => {
    window.sent = [];
    const sender = (refusal, read) => async (what) => {
      if (window[refusal]) throw new DOMException('refused', window[refusal]);
      window.sent.push(read(what));
    };
    Object.defineProperty(navigator, 'clipboard', { value: { writeText: sender('refuseCopy', (text) => text) } });
    Object.defineProperty(navigator, 'share', { value: sender('refuseShare', (data) => data.url) });
  });
  const said = (text) => page.waitForFunction((t) => document.getElementById('shareSaid').textContent.includes(t), text, { timeout: 30000 });
  const lastSent = () => page.evaluate(() => window.sent[window.sent.length - 1]);
  const answer = phone ? 'Link shared' : 'Link copied';
  const shareBtn = page.locator('#shareBtn'), shareField = page.locator('#shareLink');
  await page.locator('#helpBtn').click();
  await shareBtn.click();
  await said(answer);
  check(true, `the Share button says what happened, in the panel that hides the messages (${answer})`);
  const sharedLink = new URL(await lastSent());
  const linked = Object.fromEntries(sharedLink.searchParams);
  check(Object.keys(linked).sort().join() === 'e,h,m,n,p,t,y', `the link holds the spot and nothing else (${sharedLink.search})`);
  check(sharedLink.origin + sharedLink.pathname === new URL(url).origin + new URL(url).pathname, 'the link points to the page itself');
  check(+linked.e === Math.floor(spot.east) && +linked.n === Math.floor(spot.north) && +linked.y === 90 && +linked.m === 0, 'the link holds where the player is');
  if (phone) {
    await page.evaluate(() => { window.refuseShare = 'AbortError'; window.sent = []; });
    await shareBtn.click();
    await sleep(500);
    check((await shows('#shareSaid')) === '' && (await page.evaluate(() => window.sent.length)) === 0, 'cancelling the share sheet does nothing more');
    await page.evaluate(() => { window.refuseShare = 'NotAllowedError'; });
    await shareBtn.click();
    await said('Link copied');
    check(true, 'a refused share sheet falls back to the clipboard');
  }
  await page.evaluate(() => { window.refuseCopy = 'NotAllowedError'; });
  await shareBtn.click();
  await said('could not be copied');
  check(await shareField.isVisible() && (await shareField.inputValue()) === sharedLink.href, 'a refused copy shows the link in a field');
  check(await page.evaluate(() => {
    const field = document.getElementById('shareLink');
    return document.activeElement === field && field.selectionEnd === field.value.length && getComputedStyle(field).userSelect === 'text';
  }), 'and the field can be selected, on a page that cannot');
  await page.evaluate(() => { window.refuseCopy = window.refuseShare = null; });
  await page.locator('#start').click();
  check(!(await shareField.isVisible()) && (await shows('#shareSaid')) === '', 'closing the panel clears the answer');
  await page.keyboard.press('KeyL');
  await noted(answer);
  check(true, 'L shares too, and says so where it can be seen');


  const end = await status();
  console.log('end', JSON.stringify(end), 'frame ms', await page.evaluate(() => window.oldTownWalk.kit.frameMs));
  check(end.peak <= 256, 'meshes stay under Strata\'s limit');
  check(errors.length === 0, 'no page errors' + (errors.length ? ': ' + errors.slice(0, 4).join(' | ') : ''));

  // Links, each followed in a page of its own, the way a person would.
  await page.close();
  const follow = async (query, newVisitor) => {
    const tab = await context.newPage(), problems = [];
    tab.on('pageerror', (e) => problems.push(String(e)));
    tab.on('console', (m) => { if (m.type() === 'error') problems.push(m.text()); });
    if (newVisitor) await tab.addInitScript(() => localStorage.removeItem('old-town-walk-seen'));
    await tab.goto(`${url}${url.includes('?') ? '&' : '?'}${query}`);
    await tab.waitForFunction(() => window.oldTownWalk && window.oldTownWalk.status().ready === 1 && window.oldTownWalk.status().arriving === 0, null, { timeout: 180000 });
    const walk = await tab.evaluate(() => ({...window.oldTownWalk.status(), sunTime: window.oldTownWalk.sunTime,
      clock: document.getElementById('timeValue').textContent, note: document.getElementById('note').textContent,
      said: document.getElementById('shareSaid').textContent, welcome: document.getElementById('help').classList.contains('on'),
      share: !document.getElementById('shareBtn').hidden, nodes: document.querySelectorAll('#note *, #shareSaid *, body > img').length}));
    await tab.close();
    check(problems.length === 0, `${query}: no page errors` + (problems.length ? ': ' + problems.slice(0, 4).join(' | ') : ''));
    return walk;
  };
  const landed = await follow(sharedLink.search.slice(1));
  check(Math.hypot(landed.east - linked.e, landed.north - linked.n) < 2 && landed.yaw === 90 && !landed.flying && !landed.riding, 'a shared link lands on the spot, on foot, facing the same way');
  const inAir = await follow('e=493300&n=4492700&y=45&p=-20&h=80&m=2&t=1290');
  check(inAir.flying === 1 && Math.abs(inAir.height - 80) < 1 && inAir.yaw === 45 && inAir.pitch === -20 && Math.hypot(inAir.east - 493300, inAir.north - 4492700) < 1, 'a link can be in the air, at its height');
  check(inAir.clock === '21:30', `and sets the time of day (${inAir.clock})`);
  const onBike = await follow('e=493300&n=4492700&y=180&m=1');
  check(onBike.riding === 1 && onBike.flying === 0 && onBike.height < 3, 'a link can be on the bike');
  const atStart = (walk) => Math.hypot(walk.east - first.east, walk.north - first.north) < 0.5 && walk.yaw === first.yaw && !walk.flying && !walk.riding && walk.sunTime === Date.UTC(2026, 6, 15, 21) / 1000;
  for (const junk of ['e=NaN&n=NaN&y=Infinity', 'e=99999999999999999999&n=1e999&h=-5', 'e=%3Cimg%20src=x%20onerror=alert(1)%3E&n=hello&t=noon', 'e=493300&n=4492700&m=1.5']) {
    const ignored = await follow(junk);
    check(atStart(ignored) && ignored.nodes === 0 && ignored.note.includes('could not be read'), `${junk} is ignored, and the player is told`);
  }
  // New visitors last: the first of them forgets, for every later page, that the controls have been seen.
  const welcomed = await follow(`e=${SOLID.east}&n=${SOLID.north}&y=10`, true);
  const moved = Math.hypot(welcomed.east - SOLID.east, welcomed.north - SOLID.north);
  check(moved > 0.5 && moved < 90, `a link into a building lands outside it (${moved.toFixed(1)} m)`);
  check(welcomed.welcome && !welcomed.share, 'and a new visitor still gets the welcome');
  const offMap = await follow('e=300000&n=4492700&m=2&t=60', true);
  check(atStart(offMap) && offMap.said.includes('off the map'), 'a spot off the map is ignored, and the player is told, above the welcome');
  await browser.close();
})().catch((e) => { console.error(e); process.exit(1); });
