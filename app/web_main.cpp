// Old Town Walk in a browser: the same tiles, walker and stream as the desktop viewer, on Strata's
// web kit. The page (web/page.js) owns the input and the downloads and calls in here once a frame.
#define STRATA_KIT_MAIN
#include "strata_kit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "tile.h"
#include "bike_view.h"
#include "place_view.h"
#include "sign_view.h"
#include "tile_stream.h"
#include "walker.h"

namespace {

// Tiles load at most this often once the player is on the map, so one upload at a time shows as a
// small hitch and not a stall.
constexpr double kLoadGapSeconds = 0.12;
// Metres around the start that must be on the GPU before the page shows the map.
constexpr float kReadyRadius = 150.0f;
// And around a place the player travels to, before they are set down on it.
constexpr float kArriveRadius = 60.0f;

struct Game {
    otw::Manifest manifest;
    otw::TileKey origin;
    std::unique_ptr<otw::Stream> stream;
    otw::Walker walker;
    std::map<otw::TileKey, otw::Tile> arrived;  // downloaded, not uploaded yet
    std::set<otw::TileKey> asked;
    bool placed = false, flying = false, ready = false;
    float flySpeed = otw::kFlySpeed, walkSpeed = otw::kWalkSpeed, bikeSpeed = otw::kBikeSpeed;
    double sinceLoad = 1;
    float startX = 0, startZ = 0;
    float camera[7] = {};  // the kit's six, then the bike's lean, which the page shows by turning the canvas
    std::string status;
    otw::SignView signs;
    otw::PlaceView places;
    int signFrames = 0;
    otw::Bike bike;
    bool hurrying = false;  // this frame, with Shift or a full stick
    bool arriving = false;  // travelled, and waiting for the map to load there
    float arriveHeight = 0;  // metres above the ground, when arriving in flight
};

Game* game = nullptr;

EM_JS(void, otw_request_tile, (int ix, int iy), { if (Module.onTileWanted) Module.onTileWanted(ix, iy); });

otw::TileFetch fetchTile(otw::TileKey k, otw::Tile& out, std::string*) {
    auto got = game->arrived.find(k);
    if (got != game->arrived.end()) {
        out = std::move(got->second);
        game->arrived.erase(got);
        game->asked.erase(k);
        return otw::TileFetch::Ready;
    }
    if (game->asked.insert(k).second) otw_request_tile(k.ix, k.iy);
    return otw::TileFetch::Pending;
}

void makeStream(StrataEngine* engine, float radius) {
    otw::StreamConfig config;
    config.loadRadius = radius;
    config.unloadRadius = radius + 200;
    game->stream = std::make_unique<otw::Stream>(engine, game->manifest, game->origin, config, fetchTile);
}

// The kit's camera orbits a target; one metre out, looking back through the target, it is an eye.
void aimCamera() {
    const otw::Walker& w = game->walker;
    float ahead[3];
    w.forward(ahead);
    game->camera[0] = std::atan2(-ahead[2], -ahead[0]);
    game->camera[1] = std::asin(std::clamp(-ahead[1], -1.0f, 1.0f));
    game->camera[2] = 1.0f;
    game->camera[3] = w.x + ahead[0];
    game->camera[4] = w.eyeY + w.hop + ahead[1];
    game->camera[5] = w.z + ahead[2];
    game->camera[6] = game->bike.lean;
}

// Everything the map has within this far of a spot.
bool loadedAround(float x, float z, float radius) {
    const otw::Stream& s = *game->stream;
    const double east = s.east(x), north = s.north(z);
    for (const otw::TileKey& k : game->manifest.tiles) {
        if (otw::tileDistance(k, east, north, game->manifest.tileSize) > radius) continue;
        const float cx = s.offsetX(k) + game->manifest.tileSize * 0.5f, cz = s.offsetZ(k) + game->manifest.tileSize * 0.5f;
        if (!s.walkable(cx, cz)) return false;
    }
    return true;
}

bool startLoaded() { return loadedAround(game->startX, game->startZ, kReadyRadius); }

}  // namespace

void strata_kit_configure(strata_kit::Options& o) {
    o.name = "old-town-walk";
    o.fovDegrees = 70.0f;
    o.nearPlane = 0.1f;
    o.farPlane = 1600.0f;
    o.shadowDistanceMin = 220.0f;
    o.shadowDistancePerDist = 0.0f;
    o.tiers[0] = {0, 1024, 0, false};
    o.tiers[1] = {1, 1024, 0, false};
    o.tiers[2] = {3, 2048, 0, false};
}

// Runs at the start and again after a lost context: nothing survives on the new one, so tiles load
// again, from the browser's cache.
void strata_kit_engine_ready(strata_kit::Host& h) {
    if (game && game->stream) game->stream->restart(h.engine);
}

void strata_kit_build(strata_kit::Host& h, double) {
    if (!game || !game->stream) return;
    const auto& trees = game->stream->trees();
    h.items.insert(h.items.end(), trees.begin(), trees.end());
    const auto& signs = game->signs.items();
    h.items.insert(h.items.end(), signs.begin(), signs.end());
    for (const otw::SignView::Label& l : game->signs.labels()) h.label(l.at[0], l.at[1], l.at[2], l.text.c_str(), otw::kSignPixels);
    for (const otw::PlaceView::Label& l : game->places.labels()) h.label(l.at[0], l.at[1], l.at[2], l.text.c_str(), otw::kPlacePixels, {1.0f, 0.85f, 0.45f});
    if (game->bike.riding) {
        const otw::Walker& w = game->walker;
        for (const otw::BikeSegment& s : otw::handlebars(game->bike, w.x, w.eyeY, w.z))
            h.wideLine(s.from[0], s.from[1], s.from[2], s.to[0], s.to[1], s.to[2], {s.grey, s.grey, s.grey}, 2.0f, s.width, 1.0f,
                       STRATA_LINES_ON_TOP | STRATA_LINES_ROUND_CAPS);
    }
    h.meshes = game->stream->meshes();
}

extern "C" {

// The manifest's text, once the engine exists. Returns 0 when it cannot be read.
EMSCRIPTEN_KEEPALIVE int otw_start(const char* manifestText, float radius) {
    game = new Game;
    if (!otw::parseManifest(manifestText, "tiles", game->manifest)) return 0;
    const otw::Manifest& m = game->manifest;
    const double east = m.startEast + otw::kStartEast, north = m.startNorth + otw::kStartNorth;
    game->origin = {static_cast<int>(std::floor(east / m.tileSize)), static_cast<int>(std::floor(north / m.tileSize))};
    game->startX = static_cast<float>(east - game->origin.ix * static_cast<double>(m.tileSize));
    game->startZ = static_cast<float>((game->origin.iy + 1) * static_cast<double>(m.tileSize) - north);
    game->walker.x = game->startX;
    game->walker.z = game->startZ;
    game->walker.yawDeg = otw::kStartYaw;
    makeStream(strata_kit::host().engine, radius);
    return 1;
}

// A tile's bytes, downloaded and unpacked by the page. Returns 0 when they are not a tile.
EMSCRIPTEN_KEEPALIVE int otw_tile(int ix, int iy, const uint8_t* bytes, int size) {
    if (!game) return 0;
    otw::Tile tile;
    std::string why;
    if (!otw::parseTile(bytes, static_cast<size_t>(size), tile, &why)) {
        std::fprintf(stderr, "old-town-walk: tile %d %d: %s\n", ix, iy, why.c_str());
        return 0;
    }
    game->arrived[{ix, iy}] = std::move(tile);
    return 1;
}

// One frame of play. Returns the six floats the kit's camera takes.
EMSCRIPTEN_KEEPALIVE const float* otw_step(float dt, float ahead, float right, float up, int run, float dyaw, float dpitch) {
    if (!game || !game->stream) return nullptr;
    otw::Stream& s = *game->stream;
    otw::Walker& w = game->walker;
    game->sinceLoad += dt;
    const bool hurry = !game->ready || game->arriving;  // nothing to see yet, so a stall costs nothing
    if (hurry || game->sinceLoad >= kLoadGapSeconds) {
        const int before = s.loads();
        if (!s.update(w.x, w.z, 1)) return nullptr;
        if (s.loads() != before) game->sinceLoad = 0;
    }
    if (!game->placed && s.walkable(w.x, w.z) && startLoaded()) {
        // Only once everything nearby has loaded: a building in a tile still on its way cannot be stepped out of.
        w.clearOfBuildings(s.solids(), [&](float x, float z) { return s.walkable(x, z); });
        w.eyeY = s.groundAt(w.x, w.z) + otw::kEyeHeight;
        game->startX = w.x;  // R comes back here
        game->startZ = w.z;
        game->placed = true;
    }
    if (!game->ready && game->placed && startLoaded()) game->ready = true;
    if (game->arriving && s.walkable(w.x, w.z) && loadedAround(w.x, w.z, kArriveRadius)) {
        if (!game->flying) w.clearOfBuildings(s.solids(), [&](float x, float z) { return s.walkable(x, z); });
        const float ground = s.groundAt(w.x, w.z);
        w.eyeY = ground + (game->flying ? std::clamp(game->arriveHeight, otw::kFlyClearance, otw::kFlyCeiling) : game->bike.riding ? otw::kBikeEye : otw::kEyeHeight);
        game->arriving = false;
    }
    game->hurrying = run != 0 && (ahead != 0 || right != 0 || (game->flying && up != 0));
    w.look(dyaw, dpitch);
    if (s.walkable(w.x, w.z) && !game->arriving) {  // false for a moment after a lost context, while tiles come back
        auto walkable = [&](float x, float z) { return s.walkable(x, z); };
        if (game->flying) {
            w.fly(walkable, [&](float x, float z) { return s.groundAt(x, z); }, ahead, right, up, run != 0, dt, game->flySpeed);
        } else if (game->bike.riding) {
            game->bike.ride(w, s.solids(), walkable, ahead, right, run != 0, dt, game->bikeSpeed);
            const float seat = s.groundAt(w.x, w.z) + otw::kBikeEye;
            w.eyeY += (seat - w.eyeY) * std::min(1.0f, dt * 10.0f);
        } else {
            w.step(s.solids(), walkable, ahead, right, run != 0, dt, game->walkSpeed);
            const float target = s.groundAt(w.x, w.z) + otw::kEyeHeight;
            w.eyeY += (target - w.eyeY) * std::min(1.0f, dt * 10.0f);
            w.stepHop(dt);
        }
    }
    if (game->placed && game->signFrames++ % 15 == 0) game->signs.update(game->manifest.streets, s, w, game->flying);
    if (game->placed && !game->arriving && game->signFrames % 15 == 1) game->places.update(game->manifest.places, s, w, game->flying);
    aimCamera();
    return game->camera;
}

// For the page's messages: 1 when a move by (ahead, right) would leave the map, 2 at the flight ceiling, 0 otherwise.
EMSCRIPTEN_KEEPALIVE int otw_limit(float ahead, float right, float rise, float reach) {
    if (!game || !game->stream) return 0;
    const otw::Stream& s = *game->stream;
    const otw::Walker& w = game->walker;
    if (game->flying && rise > 0 && w.eyeY - s.groundAt(w.x, w.z) >= otw::kFlyCeiling - 0.5f) return 2;
    float dx, dz;
    otw::travelDirection(w, game->bike, ahead, right, dx, dz);
    if (dx == 0 && dz == 0) return 0;
    return s.overTile(w.x + dx * reach, w.z + dz * reach) ? 0 : 1;
}

EMSCRIPTEN_KEEPALIVE void otw_jump() {
    if (game && !game->flying && !game->bike.riding) game->walker.jump();
}

EMSCRIPTEN_KEEPALIVE void otw_set_speeds(float fly, float walk) {
    if (!game) return;
    game->flySpeed = fly;
    game->walkSpeed = walk;
}

EMSCRIPTEN_KEEPALIVE void otw_go_to_start() {
    if (!game || !game->stream) return;
    otw::Walker& w = game->walker;
    w.x = game->startX;
    w.z = game->startZ;
    w.yawDeg = otw::kStartYaw;
    w.pitchDeg = 0;
    w.hop = w.hopSpeed = 0;
    game->flying = false;
    game->arriving = false;
    game->bike.dismount();
    w.eyeY = game->stream->groundAt(w.x, w.z) + otw::kEyeHeight;
}

// Moves the player to a spot, in manifest eastings and northings, for the map, landmarks and shared links. mode is 0 on
// foot, 1 on the bike, 2 flying, height metres above the ground. Returns 0 for a spot off the map. The map may have to
// load first, so the player is set down, clear of any building, a few frames later; the status says "arriving" until then.
EMSCRIPTEN_KEEPALIVE int otw_travel(double east, double north, float yaw, float pitch, int mode, float height) {
    if (!game || !game->stream) return 0;
    const otw::Stream& s = *game->stream;
    const float x = static_cast<float>(east - s.east(0)), z = static_cast<float>(s.north(0) - north);
    if (!s.overTile(x, z)) return 0;
    otw::Walker& w = game->walker;
    w.x = x;
    w.z = z;
    w.yawDeg = std::fmod(std::fmod(yaw, 360.0f) + 360.0f, 360.0f);
    w.pitchDeg = std::clamp(pitch, -85.0f, 85.0f);
    w.hop = w.hopSpeed = 0;
    game->flying = mode == 2;
    game->bike.dismount();
    if (mode == 1) game->bike.mount(w);
    game->arriveHeight = height;
    game->arriving = true;
    return 1;
}

// Gets on or off the bike, landing first if need be. Returns 1 when riding.
EMSCRIPTEN_KEEPALIVE int otw_toggle_bike() {
    if (!game || !game->stream) return 0;
    otw::Stream& s = *game->stream;
    if (game->flying) {
        game->flying = false;
        game->walker.clearOfBuildings(s.solids(), [&](float x, float z) { return s.walkable(x, z); });
    }
    if (game->bike.riding) game->bike.dismount();
    else game->bike.mount(game->walker);
    return game->bike.riding ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void otw_set_bike_speed(float speed) {
    if (game) game->bikeSpeed = speed;
}

// Vertical degrees, as on the desktop. The kit reads its options afresh every frame and keeps them through a lost
// context, so this is all a field of view that changes in play needs. The page calls it once a frame.
EMSCRIPTEN_KEEPALIVE void otw_set_fov(float degrees) {
    strata_kit::host().opts.fovDegrees = degrees;
}

EMSCRIPTEN_KEEPALIVE int otw_toggle_fly() {
    if (!game || !game->stream) return 0;
    otw::Stream& s = *game->stream;
    game->flying = !game->flying;
    game->bike.dismount();
    if (game->flying) {
        game->walker.hop = game->walker.hopSpeed = 0;
        game->walker.eyeY += otw::kFlyClearance;  // lift off at once
    } else {
        game->walker.clearOfBuildings(s.solids(), [&](float x, float z) { return s.walkable(x, z); });
    }
    return game->flying ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void otw_set_radius(float radius) {
    if (game && game->stream) game->stream->setLoadRadius(radius);
}

// For the page and its tests: where the player is and what is loaded.
EMSCRIPTEN_KEEPALIVE const char* otw_status() {
    if (!game || !game->stream) return "{}";
    const otw::Stream& s = *game->stream;
    const otw::Walker& w = game->walker;
    char text[640];
    std::snprintf(text, sizeof text,
                  "{\"ready\":%d,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"yaw\":%.1f,\"pitch\":%.1f,\"flying\":%d,\"tiles\":%d,"
                  "\"meshes\":%d,\"peak\":%d,\"loads\":%d,\"unloads\":%d,\"worstMs\":%.1f,\"waiting\":%d,\"height\":%.1f,\"street\":\"%s\",\"signs\":%d,\"riding\":%d,\"speed\":%.1f,\"pace\":%.2f,\"hurrying\":%d,\"fov\":%.1f,\"east\":%.2f,\"north\":%.2f,\"arriving\":%d}",
                  game->ready ? 1 : 0, static_cast<double>(w.x), static_cast<double>(w.eyeY), static_cast<double>(w.z),
                  static_cast<double>(w.yawDeg), static_cast<double>(w.pitchDeg), game->flying ? 1 : 0, s.tileCount(), s.meshes(),
                  s.peakMeshes(), s.loads(), s.unloads(), s.worstLoadMs(), s.waiting(), static_cast<double>(w.eyeY + w.hop - s.groundAt(w.x, w.z)),
                  game->signs.street().c_str(), static_cast<int>(game->signs.labels().size()), game->bike.riding ? 1 : 0, static_cast<double>(game->bike.speed),
                  static_cast<double>(otw::shownSpeed(game->flying, game->bike, game->flying ? game->flySpeed : game->walkSpeed, game->hurrying)), game->hurrying ? 1 : 0,
                  static_cast<double>(strata_kit::host().opts.fovDegrees), s.east(w.x), s.north(w.z), game->arriving ? 1 : 0);
    game->status = text;
    game->status.pop_back();  // the closing brace: a place's name has no set length
    game->status += ",\"place\":\"" + game->places.here() + "\",\"arrivals\":" + std::to_string(game->places.arrivals()) + "}";
    return game->status.c_str();
}

}  // extern "C"
