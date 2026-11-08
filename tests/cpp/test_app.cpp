// Unit tests for everything that needs no GPU: the tile and manifest readers, streaming and walking.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "otw.h"
#include "bike_view.h"
#include "exe_dir.h"
#include "places.h"
#include "night.h"
#include "settings.h"
#include "streaming.h"
#include "tile.h"
#include "walker.h"

static int failures = 0;
static std::string tilesDir = "build/tiles";
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

using otw::TileKey;

static std::vector<TileKey> grid(int across) {
    std::vector<TileKey> out;
    for (int y = 0; y < across; ++y) {
        for (int x = 0; x < across; ++x) out.push_back({x, y});
    }
    return out;
}

static bool has(const std::vector<TileKey>& v, TileKey k) { return std::find(v.begin(), v.end(), k) != v.end(); }

static void testStreaming() {
    CHECK(otw::tileDistance({0, 0}, 250, 250, 500) == 0);
    CHECK(std::fabs(otw::tileDistance({1, 0}, 250, 250, 500) - 250) < 1e-9);
    CHECK(std::fabs(otw::tileDistance({1, 1}, 400, 400, 500) - std::hypot(100.0, 100.0)) < 1e-9);

    otw::StreamConfig c;
    c.loadRadius = 300;
    c.unloadRadius = 400;
    c.maxTiles = 60;
    const auto all = grid(10);
    // From nothing: the tile underfoot comes first, then only what is in range.
    auto p = otw::planStreaming(all, {}, 2250, 2250, 500, c);
    CHECK(!p.load.empty() && p.load[0] == (TileKey{4, 4}));
    CHECK(p.load.size() == 5 && p.unload.empty());  // the tile and its four sides; corners are 353 m off
    CHECK(!has(p.load, {2, 4}));

    // Loaded tiles are not asked for again.
    std::set<TileKey> loaded(p.load.begin(), p.load.end());
    CHECK(otw::planStreaming(all, loaded, 2250, 2250, 500, c).load.empty());

    // Hysteresis: a tile past the load radius but inside the unload radius stays, so an edge walk does not thrash.
    std::set<TileKey> one{{0, 0}};
    CHECK(otw::planStreaming(all, one, 850, 250, 500, c).unload.empty());
    CHECK(has(otw::planStreaming(all, one, 901, 250, 500, c).unload, {0, 0}));

    // The budget caps what loads, keeps the nearest, and counts tiles being unloaded as free.
    c.maxTiles = 2;
    p = otw::planStreaming(all, {}, 2250, 2250, 500, c);
    CHECK(p.load.size() == 2 && p.load[0] == (TileKey{4, 4}));
    std::set<TileKey> full{{9, 9}, {4, 4}};
    p = otw::planStreaming(all, full, 2250, 2250, 500, c);
    CHECK(p.unload.size() == 1 && p.load.size() == 1);

    // Off the data: nothing to load, and no crash.
    c.maxTiles = 60;
    CHECK(otw::planStreaming(all, {}, -5000, -5000, 500, c).load.empty());
}

static otw::Tile squareTile() {
    otw::Tile t;
    t.key = {3, 7};
    t.size = 20;
    t.cell = 1;
    t.samples = 21;
    for (int r = -1; r <= 21; ++r) {
        for (int c = -1; c <= 21; ++c) t.heights.push_back(static_cast<float>(c) * 0.1f);
    }
    t.footprints.push_back({{8, 8, 12, 8, 12, 12, 8, 12}});
    t.trees.push_back({4, 0, 4, 9, 3, 0, 1, 0});
    return t;
}

static void testWalking() {
    const otw::Tile t = squareTile();
    CHECK(std::fabs(t.heightAt(5, 5) - 0.5f) < 1e-5f);
    CHECK(std::fabs(t.heightAt(5.5f, 2) - 0.55f) < 1e-5f);
    otw::Solids solids;
    solids.add(t, 100, 200);
    CHECK(solids.inside(110, 210) && !solids.inside(107, 210));
    auto anywhere = [](float, float) { return true; };

    {
        // Flight: over walls, toward the view, never into the ground, and only as far as the map goes.
        auto ground = [](float x, float) { return x * 0.01f; };
        auto mapped = [](float x, float) { return x < 1000.0f; };
        otw::Walker f;
        f.x = 100;
        f.z = 210;
        f.eyeY = 50;
        f.yawDeg = 90;
        f.fly(mapped, ground, 1, 0, 0, false, 1.0f);
        CHECK(std::fabs(f.x - (100 + otw::kFlySpeed)) < 1e-3f && std::fabs(f.eyeY - 50) < 1e-3f);
        CHECK(solids.inside(110, 210));  // the flight above went straight through this building
        f.fly(mapped, ground, 1, 0, 0, true, 1.0f);
        CHECK(std::fabs(f.x - (120 + otw::kFlyFastSpeed)) < 1e-3f);
        f.fly(mapped, ground, 0, 1, 0, false, 1.0f);  // right of east is south
        CHECK(std::fabs(f.z - (210 + otw::kFlySpeed)) < 1e-3f);
        f.pitchDeg = -45;  // diving goes down as well as along
        const float before = f.eyeY;
        f.fly(mapped, ground, 1, 0, 0, false, 0.5f);
        CHECK(f.eyeY < before);
        f.pitchDeg = 0;
        f.fly(mapped, ground, 0, 0, -1, true, 100.0f);
        CHECK(std::fabs(f.eyeY - (ground(f.x, f.z) + otw::kFlyClearance)) < 1e-3f);
        f.fly(mapped, ground, 0, 0, 1, true, 100.0f);
        CHECK(std::fabs(f.eyeY - (ground(f.x, f.z) + otw::kFlyCeiling)) < 1e-3f);
        f.fly(mapped, ground, 1, 0, 0, true, 100.0f);
        CHECK(f.x < 1000);  // the edge of the map stops a flight
        const float z0 = f.z;
        f.yawDeg = 135;  // and it can be slid along
        f.fly(mapped, ground, 1, 0, 0, false, 1.0f);
        CHECK(f.z > z0 && f.x < 1000);

        {
            // A jump rises about a metre, comes down, and cannot be repeated in the air.
            otw::Walker j;
            j.jump();
            float top = 0;
            int frames = 0;
            for (; frames < 300 && (frames == 0 || j.hop > 0); ++frames) {
                if (frames == 10) j.jump();
                j.stepHop(1.0f / 60.0f);
                top = std::max(top, j.hop);
            }
            CHECK(top > 1.0f && top < 1.3f);
            CHECK(j.hop == 0 && j.hopSpeed == 0 && frames > 40 && frames < 70);
            j.jump();
            CHECK(j.hopSpeed == otw::kJumpSpeed);
        }
        otw::Walker quick;  // A chosen speed, and Shift scaling it.
        quick.x = 100;
        quick.yawDeg = 90;
        quick.fly(mapped, ground, 1, 0, 0, false, 1.0f, 200.0f);
        CHECK(std::fabs(quick.x - 300) < 1e-2f);
        quick.fly(mapped, ground, 1, 0, 0, true, 1.0f, 100.0f);
        CHECK(std::fabs(quick.x - 700) < 1e-2f);
        CHECK(otw::stepViewRadius(700, 1) == 800.0f && otw::stepViewRadius(700, -1) == 600.0f);
        CHECK(otw::stepViewRadius(1500, 1) == otw::kMaxViewRadius && otw::stepViewRadius(200, -1) == otw::kMinViewRadius);
        CHECK(otw::stepViewRadius(733, 1) == 800.0f);  // a --radius between steps lands on one
        {
            // The longest view fits Strata's mesh limit anywhere on a full map, with nothing dropped by the budget.
            std::vector<otw::TileKey> all;
            for (int y = 0; y < 40; ++y)
                for (int x = 0; x < 40; ++x) all.push_back({x, y});
            otw::StreamConfig longest;
            longest.loadRadius = otw::kMaxViewRadius;
            longest.unloadRadius = otw::kMaxViewRadius + 200;
            const auto plan = otw::planStreaming(all, {}, 10000, 10000, 500, longest);
            size_t reach = 0;
            for (const auto& k : all) reach += otw::tileDistance(k, 10000, 10000, 500) <= otw::kMaxViewRadius;
            CHECK(reach <= static_cast<size_t>(longest.maxTiles) && plan.load.size() == reach);
        }
        CHECK(otw::stepFov(70, 1) == 75.0f && otw::stepFov(110, 1) == otw::kMaxFov && otw::stepFov(40, -1) == otw::kMinFov);
        {
            // Settings come back as they went in, and a bad file cannot do harm.
            otw::Settings chosen;
            chosen.flySpeed = 152.59f;
            chosen.viewRadius = 1200;
            chosen.sensitivity = 0.075f;
            chosen.fov = 95;
            chosen.panel = false;
            chosen.invertLook = true;
            const otw::Settings back = otw::parseSettings(otw::formatSettings(chosen));
            CHECK(std::fabs(back.flySpeed - chosen.flySpeed) < 0.01f && back.viewRadius == 1200 && back.fov == 95 && !back.panel);
            CHECK(std::fabs(back.sensitivity - chosen.sensitivity) < 0.001f && back.invertLook);
            const otw::Settings bad = otw::parseSettings("fly_speed 99999\nfov banana\nmystery 3\nview_radius -5\n");
            CHECK(bad.flySpeed == otw::kMaxFlySpeed && bad.fov == 70 && bad.viewRadius == otw::kMinViewRadius);
            CHECK(otw::parseSettings("").flySpeed == 20);
            // Reduced motion is off until asked for, and the longest possible file still reaches it.
            CHECK(!otw::Settings{}.reduceMotion && !otw::parseSettings("").reduceMotion);
            CHECK(otw::parseSettings("reduce_motion 1\n").reduceMotion && !otw::parseSettings("reduce_motion 0\n").reduceMotion);
            CHECK(!otw::parseSettings("reduce_motion banana\n").reduceMotion);
            CHECK(!otw::parseSettings(otw::formatSettings(otw::Settings{})).reduceMotion);
            otw::Settings longest;
            longest.flySpeed = otw::kMaxFlySpeed;
            longest.walkSpeed = otw::kMaxWalkSpeed;
            longest.viewRadius = otw::kMaxViewRadius;
            longest.sensitivity = otw::kMaxSensitivity;
            longest.fov = otw::kMaxFov;
            longest.bikeSpeed = otw::kMaxBikeSpeed;
            longest.reduceMotion = true;
            const otw::Settings kept = otw::parseSettings(otw::formatSettings(longest));
            CHECK(kept.reduceMotion && kept.bikeSpeed == otw::kMaxBikeSpeed);
        }
        CHECK(otw::frameStep(0.016f) == 0.016f && otw::frameStep(0.2f) == 0.2f && otw::frameStep(3.0f) == 0.25f);
        CHECK(std::string(otw::compass(0)) == "N" && std::string(otw::compass(359)) == "N");
        CHECK(std::string(otw::compass(90)) == "E" && std::string(otw::compass(225)) == "SW");
        const std::string panel = otw::hudText({true, 40, 700, 120, 90, 58, 11, 0, 0.5f});
        CHECK(panel.find("clock   12:00") != std::string::npos);
        CHECK(otw::hudText({true, 40, 700, 120, 90, 58, 11, 0, 0.75f}).find("clock   18:00") != std::string::npos);
        CHECK(panel.find("flying  40 m/s") != std::string::npos && panel.find("view    700 m") != std::string::npos);
        CHECK(panel.find("facing  E") != std::string::npos && panel.find("tiles   11") != std::string::npos);
        CHECK(otw::hudText({false, 1.4f, 700, 1.65f, 0, 60, 4, 0, 0.5f}).find("on foot  1.4 m/s") != std::string::npos);
        otw::HudState running{false, 5, 700, 1.65f, 0, 60, 4, 0, 0.5f};
        running.running = true;
        CHECK(otw::hudText(running).find("running  5.0 m/s") != std::string::npos);
        CHECK(otw::hudText({true, 40, 700, 120, 90, 58, 11, 3, 0.5f}).find("tiles   11 +3 loading") != std::string::npos);
        CHECK(panel.find("loading") == std::string::npos);
        CHECK(otw::stepSpeed(20, 1, 5, 800) == 25.0f);
        CHECK(std::fabs(otw::stepSpeed(25, -1, 5, 800) - 20.0f) < 1e-4f);
        CHECK(otw::stepSpeed(790, 1, 5, 800) == 800.0f && otw::stepSpeed(5, -1, 5, 800) == 5.0f);

        otw::Walker land;  // Landing on a roof steps off it.
        land.x = 110;
        land.z = 210;
        CHECK(solids.inside(land.x, land.z));
        land.clearOfBuildings(solids, anywhere);
        CHECK(!solids.inside(land.x, land.z));
    }

    otw::Walker p;
    p.x = 102;
    p.z = 210;
    p.yawDeg = 90;
    float f[3];
    p.forward(f);
    CHECK(f[0] > 0.99f);
    for (int i = 0; i < 60 * 10; ++i) p.step(solids, anywhere, 1, 0, false, 1.0f / 60.0f);
    CHECK(std::fabs(p.x - (108 - otw::kBodyRadius)) < 1e-3f);  // stopped at the wall
    p.step(solids, anywhere, 1, 1, true, 0.3f);                   // a long frame slides along it
    CHECK(p.z > 210.5f && p.x < 108 && !solids.inside(p.x, p.z));

    otw::Walker q;
    q.x = 101;
    q.z = 204;
    q.yawDeg = 90;
    for (int i = 0; i < 60 * 5; ++i) q.step(solids, anywhere, 1, 0, false, 1.0f / 60.0f);
    CHECK(std::fabs(q.x - (104 - otw::kTrunkRadius - otw::kBodyRadius)) < 1e-3f);  // a trunk

    otw::Walker e;  // Pace, and the edge of what is loaded.
    e.x = 0;
    e.yawDeg = 90;
    for (int i = 0; i < 60; ++i) e.step(solids, anywhere, 1, 0, false, 1.0f / 60.0f);
    CHECK(std::fabs(e.x - otw::kWalkSpeed) < 1e-3f);
    for (int i = 0; i < 60; ++i) e.step(solids, [](float x, float) { return x < 2.0f; }, 1, 0, true, 1.0f / 60.0f);
    CHECK(e.x < 2.0f && e.x > 1.8f);

    // A tile's walls leave with it.
    solids.remove(t.key);
    CHECK(!solids.inside(110, 210));
}

static void testShownSpeed() {
    otw::Bike bike;
    CHECK(otw::shownSpeed(false, bike, 1.4f, false) == 1.4f);
    CHECK(std::fabs(otw::shownSpeed(false, bike, 1.4f, true) - 5.0f) < 1e-4f);
    CHECK(otw::shownSpeed(true, bike, 20, true) == 80.0f);
    bike.riding = true;
    bike.speed = -1.5f;
    CHECK(otw::shownSpeed(false, bike, 12, true) == 1.5f);
}

static void testTravelDirection() {
    otw::Walker w;
    w.yawDeg = 90;  // looking east
    otw::Bike bike;
    float dx = 0, dz = 0;
    otw::travelDirection(w, bike, 1, 0, dx, dz);
    CHECK(std::fabs(dx - 1) < 1e-4f && std::fabs(dz) < 1e-4f);
    bike.mount(w);
    bike.heading = 0;  // riding north, still looking east
    otw::travelDirection(w, bike, 1, 1, dx, dz);
    CHECK(dx == 0 && dz == 0);  // nothing rolling, whatever is held
    bike.speed = 5;
    otw::travelDirection(w, bike, 0, 0, dx, dz);
    CHECK(std::fabs(dx) < 1e-4f && std::fabs(dz + 1) < 1e-4f);  // north, coasting
    bike.speed = -1;
    otw::travelDirection(w, bike, 0, 0, dx, dz);
    CHECK(std::fabs(dz - 1) < 1e-4f);
}

static void testBike() {
    otw::Solids none;
    auto open = [](float, float) { return true; };
    otw::Walker w;
    otw::Bike b;
    b.mount(w);
    CHECK(b.riding);
    for (int i = 0; i < 60; ++i) b.ride(w, none, open, 1, 0, false, 0.05f);
    CHECK(b.speed > 4 && b.speed <= otw::kBikeSpeed + 0.01f);
    CHECK(std::abs(w.x) < 0.01f && w.z < -8);  // north, straight ahead
    const float rolling = b.speed;
    b.ride(w, none, open, 0, 0, false, 0.5f);
    CHECK(b.speed < rolling && b.speed > 0);  // coasts
    for (int i = 0; i < 40; ++i) b.ride(w, none, open, 1, 0, true, 0.05f);
    CHECK(b.speed > otw::kBikeSpeed);  // Shift goes faster
    otw::Walker turning;
    otw::Bike t;
    t.mount(turning);
    t.ride(turning, none, open, 0, 1, false, 1);
    CHECK(t.heading == 0);  // a stopped bike does not turn on the spot
    for (int i = 0; i < 40; ++i) t.ride(turning, none, open, 1, 1, false, 0.05f);
    CHECK(t.heading > 20 && t.heading < 180 && t.lean > 0);  // an arc to the right, leaning into it
    CHECK(turning.x > 0.1f);
    for (int i = 0; i < 60; ++i) b.ride(w, none, open, -1, 0, false, 0.05f);
    CHECK(b.speed < 0 && b.speed >= -otw::kBikeReverse);  // brakes, then a slow reverse
    otw::Walker looking;
    otw::Bike v;
    v.mount(looking);
    looking.look(30, 0);
    for (int i = 0; i < 40; ++i) v.ride(looking, none, open, 1, 0, false, 0.05f);
    CHECK(std::abs(looking.yawDeg - 30) < 0.01f);  // only the rider moves the view
    for (int i = 0; i < 20; ++i) v.ride(looking, none, open, 1, 1, false, 0.05f);
    CHECK(std::abs(std::remainder(looking.yawDeg - v.heading, 360.0f) - 30) < 0.01f);  // and it turns with the bike
    otw::Walker atEdge;
    otw::Bike e;
    e.mount(atEdge);
    e.speed = 5;
    e.ride(atEdge, none, [](float, float) { return false; }, 1, 0, false, 0.1f);
    CHECK(e.speed == 0);  // a wall stops it
    CHECK(otw::hudText({false, 4.2f, 700, 1.75f, 0, 60, 4, 0, 0.5f, "", true}).find("riding  4.2 m/s") != std::string::npos);
    CHECK(otw::handlebars(b, 0, 2, 0).size() == 4);
    otw::Settings saved;
    saved.bikeSpeed = 8;
    CHECK(otw::parseSettings(otw::formatSettings(saved)).bikeSpeed == 8);
}

static void testStreets() {
    otw::Manifest m;
    CHECK(otw::parseManifest("old-town-walk-manifest 1\ntile_size 500\ntile 1 2 3 abc\nstreet 0 Linden St\nstreet 1 E Mountain Ave\n"
                             "road 0 0 0 100 0\nroad 1 50 -40 50 40\nsign 54.0 6.0 0 1\n", "x", m));
    const otw::Streets& s = m.streets;
    CHECK(s.names.size() == 2 && s.roads.size() == 2 && s.signs.size() == 1);
    CHECK(s.names[1] == "E Mountain Ave");
    CHECK(s.streetAt(10, 5, 35) == 0);
    CHECK(s.streetAt(52, 30, 35) == 1);
    CHECK(s.streetAt(10, 100, 35) == -1);
    CHECK(s.signText(s.signs[0]) == "LINDEN ST / E MOUNTAIN AVE");
    auto always = [](const otw::StreetSign&) { return true; };
    auto never = [](const otw::StreetSign&) { return false; };
    CHECK(s.signsNear(50, 0, 20, 6, always).size() == 1);
    CHECK(s.signsNear(500, 0, 20, 6, always).empty());
    CHECK(s.signsNear(50, 0, 20, 6, never).empty());
    CHECK(otw::hudText({false, 1.4f, 700, 1.65f, 0, 60, 4, 0, 0.5f, "Linden St"}).find("street  Linden St") != std::string::npos);
    CHECK(otw::hudText({false, 1.4f, 700, 1.65f, 0, 60, 4, 0, 0.5f}).find("street  -") != std::string::npos);
}

static void testPlaces() {
    otw::Manifest m;
    CHECK(otw::parseManifest("old-town-walk-manifest 1\ntile_size 500\ntile 1 2 3 abc\n"
                             "place 100.0 0.0 Old Town Square|Once a street, it became a plaza.\n"
                             "place 170.5 0.0 Avery Block|A corner block.\n"
                             "place 900.0 0.0 No sentence\nplace broken\nsomething new the viewer has not met\n", "x", m));
    CHECK(m.places.size() == 2);
    CHECK(m.tiles.size() == 1);
    const otw::Place& square = m.places[0];
    CHECK(square.name == "Old Town Square" && square.sentence == "Once a street, it became a plaza.");
    CHECK(m.places[1].east == 170.5);

    const auto close = otw::placesNear(m.places, 150, 0, 60, 5);
    CHECK(close.size() == 2 && close[0]->name == "Avery Block");
    CHECK(otw::placesNear(m.places, 150, 0, 60, 1).size() == 1);
    CHECK(otw::placesNear(m.places, 0, 900, 60, 5).empty());

    otw::PlaceWatch beginning;  // starting in a place is not arriving at it, and it stays the place you are at
    CHECK(beginning.update(m.places, 100, 5) == nullptr);
    CHECK(beginning.here() == &m.places[0] && beginning.arrivals() == 0);

    otw::PlaceWatch watch;
    CHECK(watch.update(m.places, -500, 0) == nullptr);  // nowhere in particular
    CHECK(watch.here() == nullptr);
    CHECK(watch.update(m.places, 85, 0) == &m.places[0]);  // arrives once,
    CHECK(watch.update(m.places, 85, 0) == nullptr);       // and says nothing more while standing there
    CHECK(watch.update(m.places, 100, 10) == nullptr);
    CHECK(watch.update(m.places, 65, 0) == nullptr);  // just out of reach, and back: still the same visit
    CHECK(watch.here() == &m.places[0]);
    CHECK(watch.update(m.places, 85, 0) == nullptr);
    CHECK(watch.update(m.places, 170, 0) == &m.places[1]);  // on to the next
    CHECK(watch.here() == &m.places[1]);
    CHECK(watch.update(m.places, 400, 0) == nullptr);  // away
    CHECK(watch.here() == nullptr);
    CHECK(watch.update(m.places, 170, 0) == &m.places[1]);  // a second visit counts
    CHECK(watch.arrivals() == 3);

    CHECK(otw::wrapped("one two three", 7) == "one two\nthree");
    CHECK(otw::wrapped("one two three", 40) == "one two three");
    CHECK(otw::wrapped("unbreakable word", 4) == "unbreakable\nword");
    CHECK(otw::wrapped("", 8).empty());
    CHECK(otw::widest("one two\nthree") == 7);
    CHECK(otw::widest("") == 0);
}

static otw::Streets straightRoad(double length) {
    otw::Streets s;
    s.addName(0, "Linden St");
    s.roads.push_back({0, {0, 0, length, 0}});
    return s;
}

// A wall of a building, as old_town_walk/mesh.py writes one: a quad, then the roof that ends the building.
static void addBuilding(otw::TileMesh& m, float x, float z) {
    const uint32_t base = static_cast<uint32_t>(m.positions.size() / 3);
    const float wall[12] = {x, 0, z, x + 30, 0, z, x + 30, 7, z, x, 7, z}, roof[12] = {x, 7, z, x + 30, 7, z, x + 30, 7, z + 9, x, 7, z + 9};
    m.positions.insert(m.positions.end(), wall, wall + 12);
    m.positions.insert(m.positions.end(), roof, roof + 12);
    for (int i = 0; i < 4; ++i) m.normals.insert(m.normals.end(), {0, 0, -127, 0});
    for (int i = 0; i < 4; ++i) m.normals.insert(m.normals.end(), {0, 127, 0, 0});
    for (uint32_t face : {base, base + 4}) m.indices.insert(m.indices.end(), {face, face + 1, face + 2, face, face + 2, face + 3});
}

static void testNight() {
    // The sun: Strata's, which has no seasons, so noon stands 90 degrees less the latitude.
    CHECK(std::fabs(otw::sunElevation(0.5f, 40) - 50) < 0.01f);
    CHECK(std::fabs(otw::sunElevation(0.0f, 40) + 50) < 0.01f);
    CHECK(std::fabs(otw::sunElevation(0.25f, 40)) < 0.01f);
    CHECK(std::fabs(otw::sunElevation(0.75f, 40)) < 0.01f);
    CHECK(std::fabs(otw::sunElevation(1.25f, 40)) < 0.01f);  // a time past midnight wraps

    // The fade: nothing by day, everything at night, and no jump between.
    CHECK(otw::nightLevel(50) == 0 && otw::nightLevel(6) == 0);
    CHECK(otw::nightLevel(-6) == 1 && otw::nightLevel(-50) == 1);
    CHECK(std::fabs(otw::nightLevel(0) - 0.5f) < 0.001f);
    float last = 0;
    for (float t = 0.5f; t <= 1.0f; t += 0.0005f) {  // noon to midnight
        const float level = otw::nightLevel(otw::sunElevation(t, 40.6f));
        CHECK(level >= last && level - last < 0.02f);
        last = level;
    }
    CHECK(last == 1);
    CHECK(otw::nightLevel(otw::sunElevation(0.3f, 40)) == otw::nightLevel(otw::sunElevation(0.7f, 40)));  // as it set, it rises

    // Lamps stand beside a road, evenly, on alternate sides.
    const otw::Streets road = straightRoad(1000);
    const auto lamps = otw::placeLamps(road);
    const size_t expected = static_cast<size_t>(1000 / otw::kLampSpacing) + 1;
    CHECK(lamps.size() == expected);
    for (size_t i = 0; i < lamps.size(); ++i) {
        CHECK(std::fabs(std::fabs(lamps[i].north) - otw::kLampSide) < 0.01);
        if (i == 0) continue;
        CHECK(std::fabs(lamps[i].east - lamps[i - 1].east - otw::kLampSpacing) < 0.01);
        CHECK(lamps[i].north * lamps[i - 1].north < 0);
    }
    // One that would land on a road crossing nearby goes to the other side, and none stands on either.
    otw::Streets crossed = road;
    crossed.roads.push_back({0, {0, 7, 1000, 7}});
    for (const otw::Lamp& l : otw::placeLamps(crossed)) CHECK(crossed.streetAt(l.east, l.north, otw::kLampKerb) < 0);
    // The same road twice gets no second row, and nothing without a name gets lamps.
    otw::Streets twice = road;
    twice.roads.push_back(road.roads[0]);
    CHECK(otw::placeLamps(twice).size() == lamps.size());
    otw::Streets unnamed;
    unnamed.roads.push_back({5, {0, 0, 500, 0}});
    CHECK(otw::placeLamps(unnamed).empty());
    // However much road there is, the count stops at the bound. Far enough apart that no road is near another.
    otw::Streets sprawl;
    sprawl.addName(0, "Long Rd");
    for (int i = 0; i < 50; ++i) sprawl.roads.push_back({0, {0, i * 40.0, 2000, i * 40.0}});
    const auto many = otw::placeLamps(sprawl);
    CHECK(many.size() == otw::kMostLamps);
    for (size_t i = 0; i + 1 < many.size(); i += 97) CHECK(std::hypot(many[i].east - many[i + 1].east, many[i].north - many[i + 1].north) >= otw::kLampGap);

    // A pool is a patch of ground; its strength dies away to its edge.
    otw::GlowMesh pool;
    otw::addPool(pool, 10, 20, [](float x, float) { return x * 0.1f; });
    CHECK(pool.vertices() == static_cast<size_t>(otw::kPoolGrid * otw::kPoolGrid) && pool.uvs.size() == pool.vertices() * 2);
    CHECK(std::fabs(pool.positions[1] - ((10 - otw::kPoolRadius) * 0.1f + otw::kPoolLift)) < 0.001f);
    CHECK(otw::poolShape(0) == 1 && otw::poolShape(1) == 0 && otw::poolShape(1.4f) == 0 && otw::poolShape(0.5f) < 1);

    // Windows: the same building lights the same way every time, on the wall itself, and not every building lights.
    otw::TileMesh town;
    for (int i = 0; i < 200; ++i) addBuilding(town, static_cast<float>(i % 20) * 40, static_cast<float>(i / 20) * 30);
    const otw::GlowMesh windows = otw::litWindows(town, 500, 1000), again = otw::litWindows(town, 500, 1000);
    CHECK(windows.positions == again.positions && windows.uvs == again.uvs);
    const size_t quads = windows.indices.size() / 6;
    // 200 buildings of 8 bays on 2 floors, a share of buildings by a share of windows.
    CHECK(quads > 200 * 16 * otw::kLitBuildings * otw::kLitWindows / 10000 / 2 && quads < 200 * 16 * otw::kLitBuildings * otw::kLitWindows / 10000 * 2);
    size_t lit = 0;
    for (int i = 0; i < 200; ++i) {
        otw::TileMesh one;
        addBuilding(one, static_cast<float>(i % 20) * 40, static_cast<float>(i / 20) * 30);
        lit += otw::litWindows(one, 0, 0).empty() ? 0 : 1;
    }
    CHECK(lit > 40 && lit < 160);
    for (size_t i = 0; i < windows.vertices(); ++i) {
        CHECK(std::fabs(std::fmod(windows.positions[i * 3 + 2] - 1000 + otw::kWindowLift, 30.0f)) < 0.01f);  // just out from the wall
        CHECK(windows.positions[i * 3 + 1] >= otw::kWindowSill && windows.positions[i * 3 + 1] <= 7);        // and within it
    }
    otw::TileMesh shed;  // too low to live in
    addBuilding(shed, 0, 0);
    for (size_t i = 7; i < 24; i += 3) shed.positions[i] *= 0.3f;
    CHECK(otw::litWindows(shed, 0, 0).empty());
    otw::TileMesh crowded;
    for (int i = 0; i < 4000; ++i) addBuilding(crowded, static_cast<float>(i), static_cast<float>(i) * 0.5f);
    CHECK(otw::litWindows(crowded, 0, 0).indices.size() / 6 <= otw::kMostWindows);
    CHECK(otw::windowStageLevel(0, 0) == 0 && otw::windowStageLevel(0, 1) == 1 && otw::windowStageLevel(otw::kWindowStages - 1, 1) == 1);
    CHECK(otw::windowStageLevel(otw::kWindowStages - 1, 0.3f) < otw::windowStageLevel(0, 0.3f));

    // Real lights: within budget, and handed on by fading, never by snapping.
    otw::LightSlots slots;
    slots.choose({1, 2, 3}, 2);
    CHECK(slots.slots().size() == 2);
    slots.settle();
    CHECK(slots.slots()[0].lamp == 1 && slots.slots()[0].level == 1 && slots.slots()[1].lamp == 2);
    std::vector<int> wanted = {7, 2};
    float seven = 0;
    for (int frame = 0; frame < 300; ++frame) {
        if (frame % 15 == 0) slots.choose(wanted, 2);
        const auto before = slots.slots();
        slots.tick(1.0f / 60.0f);
        for (size_t i = 0; i < 2; ++i) {
            if (slots.slots()[i].lamp == before[i].lamp) CHECK(std::fabs(slots.slots()[i].level - before[i].level) <= 1.0f / 60.0f / otw::LightSlots::kFadeSeconds + 0.0001f);
            else CHECK(before[i].level < 0.02f && slots.slots()[i].level < 0.02f);  // a slot only changes hands dark
            if (slots.slots()[i].lamp == 7) seven = slots.slots()[i].level;
        }
    }
    CHECK(seven == 1);
    CHECK(slots.slots()[1].lamp == 2 && slots.slots()[1].level == 1);  // and the one that stayed wanted never flickered
}

static void testTreesNear() {
    struct Item { float position[3]; };
    std::vector<Item> all, out;
    for (int i = 0; i < 300; ++i) all.push_back({{static_cast<float>(i * 3), 0, 0}});   // a row of trees, 3 m apart
    otw::nearItems(all, 0, 0, 180, 600, out);
    int inner = 0, outer = 0;   // not near and far: Windows headers define those
    for (const Item& item : out) (item.position[0] <= 180 ? inner : outer)++;
    CHECK(inner == 61);                    // every tree out to 180 m
    CHECK(outer > 30 && outer < 110);          // about one in three of the 140 between 180 and 600 m
    for (const Item& item : out) CHECK(item.position[0] <= 600);
    otw::nearItems(all, 0, 0, 180, 600, out);
    CHECK(static_cast<int>(out.size()) == inner + outer);   // the same trees every frame
    CHECK(otw::treeReach(0) == 300 && otw::treeReach(1000) == 800);
}

static void testFiles() {
    otw::Manifest m;
    std::string err;
    CHECK(!otw::loadManifest("no-such-dir", m, &err) && !err.empty());
    otw::Tile t;
    CHECK(!otw::loadTile("no-such.tile", t, &err));

    const std::string dir = tilesDir;
    if (!otw::loadManifest(dir, m, &err)) {
        std::puts("build/tiles not built; skipping the real-tile checks");
        return;
    }
    CHECK(m.tileSize == 500 && m.tiles.size() == 16 && std::fabs(m.latitude - 40.6) < 1);
    const TileKey k = m.tiles[5];
    CHECK(otw::loadTile(m.path(k), t, &err));
    CHECK(t.key == k && t.samples == 251 && t.meshes.size() >= 1);
    CHECK(t.heights.size() == 253u * 253u && t.colors.size() == 251u * 251u * 3);

    // A truncated copy is refused.
    std::FILE* in = std::fopen(m.path(k).c_str(), "rb");
    std::fseek(in, 0, SEEK_END);
    std::string bytes(static_cast<size_t>(std::ftell(in)), '\0');
    std::fseek(in, 0, SEEK_SET);
    CHECK(std::fread(&bytes[0], 1, bytes.size(), in) == bytes.size());
    std::fclose(in);
    std::FILE* out = std::fopen("truncated.tile", "wb");
    std::fwrite(bytes.data(), 1, bytes.size() / 2, out);
    std::fclose(out);
    otw::Tile bad;
    CHECK(!otw::loadTile("truncated.tile", bad, &err));
    // The browser reads the same bytes from memory: whole, they parse; cut short or foreign, they do not.
    otw::Tile fromMemory;
    CHECK(otw::parseTile(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), fromMemory, &err));
    CHECK(fromMemory.key == k && fromMemory.samples == t.samples && fromMemory.trees.size() == t.trees.size());
    CHECK(!otw::parseTile(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size() / 2, fromMemory, &err));
    CHECK(!otw::parseTile(reinterpret_cast<const uint8_t*>("not a tile at all"), 17, fromMemory, &err));
    otw::Manifest none;
    CHECK(!otw::parseManifest("nothing here", "tiles", none));
    CHECK(otw::parseManifest("old-town-walk-manifest 1\ntile_size 500\ntile 1 2\n", "tiles", none));
    CHECK(none.tiles.size() == 1 && none.path({1, 2}) == "tiles/1_2.tile");
    std::remove("truncated.tile");

    // Neighbours agree along their shared edge, where a seam would show.
    if (m.has({k.ix + 1, k.iy})) {
        otw::Tile east;
        CHECK(otw::loadTile(m.path({k.ix + 1, k.iy}), east, &err));
        float worst = 0;
        for (int r = 0; r < t.samples; ++r) worst = std::max(worst, std::fabs(t.height(t.samples - 1, r) - east.height(0, r)));
        CHECK(worst == 0);
    }
}

static void testFindTiles() {
    namespace fs = std::filesystem;
    const fs::path top = fs::temp_directory_path() / "old_town_walk_find_tiles";
    fs::remove_all(top);
    fs::create_directories(top / "unzipped" / "tiles");
    fs::create_directories(top / "build" / "app" / "Release");
    fs::create_directories(top / "build" / "tiles");
    std::ofstream(top / "unzipped" / "tiles" / "manifest.txt") << "x";
    std::ofstream(top / "build" / "tiles" / "manifest.txt") << "x";
    CHECK(fs::path(otw::tilesNear(top / "unzipped" / "old-town-walk.exe")) == top / "unzipped" / "tiles");
    CHECK(fs::path(otw::tilesNear(top / "build" / "app" / "old-town-walk")) == top / "build" / "tiles");
    CHECK(fs::path(otw::tilesNear(top / "build" / "app" / "Release" / "old-town-walk.exe")) == top / "build" / "tiles");
    fs::create_directories(top / "empty" / "a" / "b" / "c");
    CHECK(otw::tilesNear(top / "empty" / "a" / "b" / "c" / "old-town-walk").empty());
    fs::remove_all(top);
}

int main(int argc, char** argv) {
    if (argc > 1) tilesDir = argv[1];
    testFindTiles();
    testStreaming();
    testWalking();
    testFiles();
    testStreets();
    testPlaces();
    testNight();
    testBike();
    testTreesNear();
    testShownSpeed();
    testTravelDirection();
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("test_app: all checks passed");
    return failures ? 1 : 0;
}
