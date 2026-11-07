// Smoke test for the browser build in headless Chromium (Playwright).
//
//   node web/smoke/smoke.cjs <url> <out-dir> [--phone]
//
// Plays the way a person does, with real keys, mouse moves and (on the phone profile) touch events
// through the browser's input layer: walks, runs into nothing silly, looks around, takes off, climbs,
// lands, and loses and gets back the graphics context. Fails on any page error, on a frame that is
// flat, and on a tile that fails to download. On the phone, every control is on screen, at least 44
// CSS px, and none overlaps another. Writes PNGs to <out-dir>.
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
  await page.addInitScript(() => {
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
  const level = await status();
  check(Math.abs(level.pitch) < 15, `starting does not swing the view (pitch ${level.pitch})`);

  if (phone) {
    const cdp = await context.newCDPSession(page);
    const touch = (type, points) => cdp.send('Input.dispatchTouchEvent', { type, touchPoints: points });
    const middle = async (sel) => {
      const b = await page.locator(sel).boundingBox();
      return { x: b.x + b.width / 2, y: b.y + b.height / 2 };
    };
    for (const sel of ['#flyBtn', '#helpBtn', '#stick', '#jumpBtn']) {
      const b = await page.locator(sel).boundingBox();
      check(b && b.width >= 44 && b.height >= 44 && b.x >= 0 && b.y >= 0 &&
        b.x + b.width <= 390 && b.y + b.height <= page.viewportSize().height, `${sel} is on screen and big enough`);
    }
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
    await page.keyboard.press('KeyR');
    await page.waitForFunction(() => document.getElementById('note').textContent.includes('start'), null, { timeout: 30000 });
    check(true, 'R goes back to the start');
    const home = await status();
    check(Math.hypot(home.x - first.x, home.z - first.z) < 0.5 && home.yaw === first.yaw, 'R returns to the spot the walk began');
    const sun = await page.evaluate(() => window.oldTownWalk.sunTime);
    await page.keyboard.press('BracketRight');
    check((await page.evaluate(() => window.oldTownWalk.sunTime)) > sun, '] moves the sun');
  }

  // Back to the start first: where a run ends depends on the machine's frame rate, and a wall or pavement filling the
  // frame has too few colours to tell a real picture from a flat one.
  await page.keyboard.press('KeyR');
  await page.waitForFunction(() => Math.abs(window.oldTownWalk.status().pitch) < 1 && window.oldTownWalk.status().flying === 0, null, { timeout: 60000 });
  await sleep(1000);

  // A lost context is rebuilt, and the map comes back.
  const held = (await status()).tiles;
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

  const end = await status();
  console.log('end', JSON.stringify(end), 'frame ms', await page.evaluate(() => window.oldTownWalk.kit.frameMs));
  check(end.peak <= 256, 'meshes stay under Strata\'s limit');
  check(errors.length === 0, 'no page errors' + (errors.length ? ': ' + errors.slice(0, 4).join(' | ') : ''));
  await browser.close();
})().catch((e) => { console.error(e); process.exit(1); });
