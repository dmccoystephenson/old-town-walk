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
#include "tile_stream.h"
#include "walker.h"

namespace {

// Tiles load at most this often once the player is on the map, so one upload at a time shows as a
// small hitch and not a stall.
constexpr double kLoadGapSeconds = 0.12;
// Metres around the start that must be on the GPU before the page shows the map.
constexpr float kReadyRadius = 150.0f;
// The map's start point stands beside a wall; a few metres south-west is the open street.
constexpr double kStartEast = -12, kStartNorth = -8;

struct Game {
    otw::Manifest manifest;
    otw::TileKey origin;
    std::unique_ptr<otw::Stream> stream;
    otw::Walker walker;
    std::map<otw::TileKey, otw::Tile> arrived;  // downloaded, not uploaded yet
    std::set<otw::TileKey> asked;
    bool placed = false, flying = false, ready = false;
    float flySpeed = otw::kFlySpeed, walkSpeed = otw::kWalkSpeed;
    double sinceLoad = 1;
    float startX = 0, startZ = 0;
    float camera[6] = {};
    std::string status;
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
}

// Everything within the ready radius of the start that the map has.
bool startLoaded() {
    const otw::Stream& s = *game->stream;
    const double east = s.east(game->startX), north = s.north(game->startZ);
    for (const otw::TileKey& k : game->manifest.tiles) {
        if (otw::tileDistance(k, east, north, game->manifest.tileSize) > kReadyRadius) continue;
        const float x = s.offsetX(k) + game->manifest.tileSize * 0.5f, z = s.offsetZ(k) + game->manifest.tileSize * 0.5f;
        if (!s.walkable(x, z)) return false;
    }
    return true;
}

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
    h.meshes = game->stream->meshes();
}

extern "C" {

// The manifest's text, once the engine exists. Returns 0 when it cannot be read.
EMSCRIPTEN_KEEPALIVE int otw_start(const char* manifestText, float radius) {
    game = new Game;
    if (!otw::parseManifest(manifestText, "tiles", game->manifest)) return 0;
    const otw::Manifest& m = game->manifest;
    const double east = m.startEast + kStartEast, north = m.startNorth + kStartNorth;
    game->origin = {static_cast<int>(std::floor(east / m.tileSize)), static_cast<int>(std::floor(north / m.tileSize))};
    game->startX = static_cast<float>(east - game->origin.ix * static_cast<double>(m.tileSize));
    game->startZ = static_cast<float>((game->origin.iy + 1) * static_cast<double>(m.tileSize) - north);
    game->walker.x = game->startX;
    game->walker.z = game->startZ;
    game->walker.yawDeg = 40;  // north-east, down the street
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
    const bool hurry = !game->ready;  // nothing is on screen yet, so a stall costs nothing
    if (hurry || game->sinceLoad >= kLoadGapSeconds) {
        const int before = s.loads();
        if (!s.update(w.x, w.z, 1)) return nullptr;
        if (s.loads() != before) game->sinceLoad = 0;
    }
    if (!game->placed && s.walkable(w.x, w.z)) {
        // A start point inside a building moves to open ground.
        for (float r = 0; r < 60 && s.solids().inside(w.x, w.z); r += 1.0f) w.x += 1.0f;
        w.eyeY = s.groundAt(w.x, w.z) + otw::kEyeHeight;
        game->placed = true;
    }
    if (!game->ready && game->placed && startLoaded()) game->ready = true;
    w.look(dyaw, dpitch);
    if (s.walkable(w.x, w.z)) {  // false for a moment after a lost context, while tiles come back
        auto walkable = [&](float x, float z) { return s.walkable(x, z); };
        if (game->flying) {
            w.fly(walkable, [&](float x, float z) { return s.groundAt(x, z); }, ahead, right, up, run != 0, dt, game->flySpeed);
        } else {
            w.step(s.solids(), walkable, ahead, right, run != 0, dt, game->walkSpeed);
            const float target = s.groundAt(w.x, w.z) + otw::kEyeHeight;
            w.eyeY += (target - w.eyeY) * std::min(1.0f, dt * 10.0f);
            w.stepHop(dt);
        }
    }
    aimCamera();
    return game->camera;
}

// For the page's messages: 1 when a move by (ahead, right) would leave the map, 2 at the flight ceiling, 0 otherwise.
EMSCRIPTEN_KEEPALIVE int otw_limit(float ahead, float right, float rise, float reach) {
    if (!game || !game->stream) return 0;
    const otw::Stream& s = *game->stream;
    const otw::Walker& w = game->walker;
    if (game->flying && rise > 0 && w.eyeY - s.groundAt(w.x, w.z) >= otw::kFlyCeiling - 0.5f) return 2;
    if (ahead == 0 && right == 0) return 0;
    const float yaw = w.yawDeg * 3.14159265f / 180.0f, fx = std::sin(yaw), fz = -std::cos(yaw);
    return s.overTile(w.x + (ahead * fx - right * fz) * reach, w.z + (ahead * fz + right * fx) * reach) ? 0 : 1;
}

EMSCRIPTEN_KEEPALIVE void otw_jump() {
    if (game && !game->flying) game->walker.jump();
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
    w.yawDeg = 40;
    w.pitchDeg = 0;
    w.hop = w.hopSpeed = 0;
    game->flying = false;
    w.eyeY = game->stream->groundAt(w.x, w.z) + otw::kEyeHeight;
}

EMSCRIPTEN_KEEPALIVE int otw_toggle_fly() {
    if (!game || !game->stream) return 0;
    otw::Stream& s = *game->stream;
    game->flying = !game->flying;
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
    char text[360];
    std::snprintf(text, sizeof text,
                  "{\"ready\":%d,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"yaw\":%.1f,\"pitch\":%.1f,\"flying\":%d,\"tiles\":%d,"
                  "\"meshes\":%d,\"peak\":%d,\"loads\":%d,\"unloads\":%d,\"worstMs\":%.1f,\"waiting\":%d,\"height\":%.1f}",
                  game->ready ? 1 : 0, static_cast<double>(w.x), static_cast<double>(w.eyeY), static_cast<double>(w.z),
                  static_cast<double>(w.yawDeg), static_cast<double>(w.pitchDeg), game->flying ? 1 : 0, s.tileCount(), s.meshes(),
                  s.peakMeshes(), s.loads(), s.unloads(), s.worstLoadMs(), s.waiting(), static_cast<double>(w.eyeY + w.hop - s.groundAt(w.x, w.z)));
    game->status = text;
    return game->status.c_str();
}

}  // extern "C"
