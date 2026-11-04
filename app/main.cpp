// old-town-walk: walk a streamed map of tiles built by `uv run old-town-walk build`.
//
//   old-town-walk [--tiles DIR] [--time 0.45] [--width W --height H] [--radius M]
//         [--east M --north M] [--yaw D --pitch D]
//         [--headless [--walk-ahead SECONDS] [--run] [--pass-east M] [--pass-north M]
//                     [--screenshot out.png] [--bench FRAMES]]
//
// Without --tiles, the tiles are the `tiles` folder beside the program, or one or two folders above it.
//
// --pass-east and --pass-north move in a straight line through everything, to prove that tiles
// load and unload on the way; the run fails if none unloaded.
//
// Controls: WASD walk, Shift run, F fly (E up, Q down), mouse or arrows look, [ ] time of day, P screenshot,
// H hides the demo label,
// Esc frees the mouse (click to take it back), Esc again quits.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "exe_dir.h"
#include "streaming.h"
#include "strata/strata.h"
#include "tile.h"
#include "walker.h"

namespace {

using Clock = std::chrono::steady_clock;
double ms(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }

constexpr int kA = 4, kD = 7, kE = 8, kF = 9, kH = 11, kP = 19, kQ = 20, kSpace = 44, kLeftCtrl = 224, kS = 22, kW = 26, kEscape = 41, kLeftBracket = 47, kRightBracket = 48,
              kRight = 79, kLeft = 80, kDown = 81, kUp = 82;

struct Args {
    std::string tiles, screenshot;  // tiles: from --tiles, or found beside the program
    bool fly = false;
    float altitude = 60;  // metres above the ground when --fly starts
    float time = 0.45f, yaw = 0, pitch = 0, radius = 700, walkAhead = 0, east = 0, north = 0, passEast = 0, passNorth = 0;
    int width = 1280, height = 720, bench = 0;
    bool headless = false, run = false;
};

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
        if (k == "--tiles") a.tiles = val();
        else if (k == "--time") a.time = static_cast<float>(std::atof(val()));
        else if (k == "--yaw") a.yaw = static_cast<float>(std::atof(val()));
        else if (k == "--pitch") a.pitch = static_cast<float>(std::atof(val()));
        else if (k == "--fly") a.fly = true;
        else if (k == "--altitude") a.altitude = static_cast<float>(std::atof(val()));
        else if (k == "--radius") a.radius = static_cast<float>(std::atof(val()));
        else if (k == "--east") a.east = static_cast<float>(std::atof(val()));
        else if (k == "--north") a.north = static_cast<float>(std::atof(val()));
        else if (k == "--pass-east") a.passEast = static_cast<float>(std::atof(val()));
        else if (k == "--pass-north") a.passNorth = static_cast<float>(std::atof(val()));
        else if (k == "--walk-ahead") a.walkAhead = static_cast<float>(std::atof(val()));
        else if (k == "--width") a.width = std::atoi(val());
        else if (k == "--height") a.height = std::atoi(val());
        else if (k == "--bench") a.bench = std::atoi(val());
        else if (k == "--headless") a.headless = true;
        else if (k == "--run") a.run = true;
        else if (k == "--screenshot") a.screenshot = val();
        else {
            std::fprintf(stderr, "unknown argument %s\n", k.c_str());
            return false;
        }
    }
    return a.width > 0 && a.height > 0;
}

bool check(StrataStatus s, const char* what) {
    if (s == STRATA_OK) return true;
    std::fprintf(stderr, "old-town-walk: %s: %s (%s)\n", what, strata_status_string(s), strata_last_error());
    return false;
}

float linear(uint8_t srgb) { return std::pow(static_cast<float>(srgb) / 255.0f, 2.2f); }

// The tiles in memory and on the GPU. World x, z are metres from the north-west corner of the start tile.
class Stream {
public:
    Stream(StrataEngine* engine, const otw::Manifest& m, otw::TileKey origin, otw::StreamConfig c)
        : engine_(engine), m_(m), origin_(origin), config_(c) {
        for (int i = 0; i < 256; ++i) linear_[i] = linear(static_cast<uint8_t>(i));
    }

    float offsetX(otw::TileKey k) const { return static_cast<float>(k.ix - origin_.ix) * m_.tileSize; }
    float offsetZ(otw::TileKey k) const { return static_cast<float>(origin_.iy - k.iy) * m_.tileSize; }
    double east(float x) const { return origin_.ix * static_cast<double>(m_.tileSize) + x; }
    double north(float z) const { return (origin_.iy + 1) * static_cast<double>(m_.tileSize) - z; }
    otw::TileKey keyAt(float x, float z) const {
        return {static_cast<int>(std::floor(east(x) / m_.tileSize)), static_cast<int>(std::floor(north(z) / m_.tileSize))};
    }

    // Applies the plan for a position, loading at most maxLoads tiles.
    bool update(float x, float z, int maxLoads) {
        std::set<otw::TileKey> loaded;
        for (const auto& t : tiles_) loaded.insert(t.first);
        const otw::StreamPlan plan =
            otw::planStreaming(m_.tiles, loaded, east(x), north(z), m_.tileSize, config_);
        for (const otw::TileKey& k : plan.unload) drop(k);
        int count = 0;
        for (const otw::TileKey& k : plan.load) {
            if (count++ >= maxLoads) break;
            if (!load(k)) return false;
        }
        return true;
    }

    bool walkable(float x, float z) const { return tiles_.count(keyAt(x, z)) != 0; }
    float groundAt(float x, float z) const {
        const otw::TileKey k = keyAt(x, z);
        const auto t = tiles_.find(k);
        if (t == tiles_.end()) return 0;
        return t->second.tile.heightAt(x - offsetX(k), z - offsetZ(k));
    }
    const otw::Solids& solids() const { return solids_; }
    const std::vector<StrataRenderItem>& trees() const { return trees_; }
    int meshes() const { return meshCount_; }
    int peakMeshes() const { return peakMeshes_; }
    int tileCount() const { return static_cast<int>(tiles_.size()); }
    int loads() const { return loads_; }
    int unloads() const { return unloads_; }
    double readMs() const { return readMs_; }
    double uploadMs() const { return uploadMs_; }
    double worstLoadMs() const { return worstLoadMs_; }

private:
    struct Loaded {
        otw::Tile tile;
        std::vector<uint32_t> ids;
    };

    bool add(const StrataMesh& mesh, Loaded& l) {
        uint32_t id = 0;
        if (!check(strata_add_mesh(engine_, &mesh, &id), "mesh")) return false;
        l.ids.push_back(id);
        ++meshCount_;
        peakMeshes_ = std::max(peakMeshes_, meshCount_);
        return true;
    }

    bool addTerrain(Loaded& l, float ox, float oz) {
        const otw::Tile& t = l.tile;
        const int n = t.samples;
        const size_t count = static_cast<size_t>(n) * static_cast<size_t>(n);
        std::vector<float> pos(count * 3), nrm(count * 3), col(count * 3);
        for (int r = 0; r < n; ++r) {
            for (int c = 0; c < n; ++c) {
                const size_t i = (static_cast<size_t>(r) * static_cast<size_t>(n) + static_cast<size_t>(c)) * 3;
                pos[i] = ox + static_cast<float>(c) * t.cell;
                pos[i + 1] = t.height(c, r);
                pos[i + 2] = oz + static_cast<float>(r) * t.cell;
                const float dx = t.height(c - 1, r) - t.height(c + 1, r), dz = t.height(c, r - 1) - t.height(c, r + 1);
                const float up = 2 * t.cell, len = std::sqrt(dx * dx + up * up + dz * dz);
                nrm[i] = dx / len;
                nrm[i + 1] = up / len;
                nrm[i + 2] = dz / len;
                for (int k = 0; k < 3; ++k) col[i + static_cast<size_t>(k)] = linear_[t.colors[i + static_cast<size_t>(k)]];
            }
        }
        std::vector<uint32_t> idx;
        idx.reserve(static_cast<size_t>(n - 1) * static_cast<size_t>(n - 1) * 6);
        for (int r = 0; r + 1 < n; ++r) {
            for (int c = 0; c + 1 < n; ++c) {
                const uint32_t a = static_cast<uint32_t>(r * n + c), b = a + 1, d = a + static_cast<uint32_t>(n), e = d + 1;
                idx.insert(idx.end(), {a, d, b, b, d, e});
            }
        }
        StrataMesh mesh{};
        mesh.positions = pos.data();
        mesh.normals = nrm.data();
        mesh.colors = col.data();
        mesh.indices = idx.data();
        mesh.vertex_count = static_cast<int32_t>(count);
        mesh.index_count = static_cast<int32_t>(idx.size());
        return add(mesh, l);
    }

    bool load(otw::TileKey k) {
        const auto t0 = Clock::now();
        Loaded l;
        std::string err;
        if (!otw::loadTile(m_.path(k), l.tile, &err)) {
            std::fprintf(stderr, "old-town-walk: %s\n", err.c_str());
            return false;
        }
        const double read = ms(t0);
        const auto t1 = Clock::now();
        const float ox = offsetX(k), oz = offsetZ(k);
        if (!addTerrain(l, ox, oz)) return false;
        for (const otw::TileMesh& m : l.tile.meshes) {
            const size_t count = m.positions.size() / 3;
            std::vector<float> pos(m.positions), nrm(count * 3), col(count * 3);
            for (size_t i = 0; i < count; ++i) {
                pos[i * 3] += ox;
                pos[i * 3 + 2] += oz;
                for (size_t c = 0; c < 3; ++c) {
                    nrm[i * 3 + c] = static_cast<float>(m.normals[i * 4 + c]) / 127.0f;
                    col[i * 3 + c] = linear_[m.colors[i * 4 + c]];
                }
            }
            StrataMesh mesh{};
            mesh.positions = pos.data();
            mesh.normals = nrm.data();
            mesh.colors = col.data();
            mesh.indices = m.indices.data();
            mesh.vertex_count = static_cast<int32_t>(count);
            mesh.index_count = static_cast<int32_t>(m.indices.size());
            if (!add(mesh, l)) return false;
        }
        solids_.add(l.tile, ox, oz);
        tiles_[k] = std::move(l);
        rebuildTrees();
        const double upload = ms(t1);
        readMs_ += read;
        uploadMs_ += upload;
        worstLoadMs_ = std::max(worstLoadMs_, read + upload);
        ++loads_;
        return true;
    }

    void drop(otw::TileKey k) {
        const auto t = tiles_.find(k);
        if (t == tiles_.end()) return;
        for (uint32_t id : t->second.ids) {
            check(strata_remove_mesh(engine_, id), "remove mesh");
            --meshCount_;
        }
        solids_.remove(k);
        tiles_.erase(t);
        ++unloads_;
        rebuildTrees();
    }

    void rebuildTrees() {
        trees_.clear();
        for (const auto& entry : tiles_) {
            const float ox = offsetX(entry.first), oz = offsetZ(entry.first);
            for (const otw::Tree& t : entry.second.tile.trees) {
                const float trunk = t.height * 0.45f, crown = t.height * 0.7f;
                trees_.push_back({STRATA_PRIMITIVE_CYLINDER, {t.x + ox, t.y + trunk * 0.5f, t.z + oz},
                                  {otw::kTrunkRadius, trunk * 0.5f, otw::kTrunkRadius}, {0.105f, 0.065f, 0.034f, 1}});
                trees_.push_back({STRATA_PRIMITIVE_SPHERE, {t.x + ox, t.y + t.height - crown * 0.5f, t.z + oz},
                                  {t.crown, crown, t.crown}, {t.r, t.g, t.b, 1}});
            }
        }
    }

    StrataEngine* engine_;
    const otw::Manifest& m_;
    otw::TileKey origin_;
    otw::StreamConfig config_;
    float linear_[256];
    std::map<otw::TileKey, Loaded> tiles_;
    otw::Solids solids_;
    std::vector<StrataRenderItem> trees_;
    int meshCount_ = 0, peakMeshes_ = 0, loads_ = 0, unloads_ = 0;
    double readMs_ = 0, uploadMs_ = 0, worstLoadMs_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse(argc, argv, a)) return 2;
    otw::Manifest manifest;
    if (a.tiles.empty()) a.tiles = otw::findTiles(argc > 0 ? argv[0] : "");
    if (a.tiles.empty()) {
        std::fprintf(stderr, "old-town-walk: no tiles folder beside the program; pass --tiles DIR\n");
        return 1;
    }
    std::string err;
    if (!otw::loadManifest(a.tiles, manifest, &err)) {
        std::fprintf(stderr, "old-town-walk: %s\n", err.c_str());
        return 1;
    }
    if (strata_abi_version() != STRATA_ABI_VERSION || strata_abi_minor() < 8) {
        std::fprintf(stderr, "old-town-walk: Strata %s is too old (needs ABI 1.8 for mouse look)\n", strata_version_string());
        return 1;
    }
    StrataEngineDesc desc{};
    desc.struct_size = sizeof desc;
    desc.abi_version = STRATA_ABI_VERSION;
    desc.surface = a.headless ? STRATA_SURFACE_HEADLESS : STRATA_SURFACE_WINDOW;
    desc.width = a.width;
    desc.height = a.height;
    desc.flags = a.headless ? 0u : static_cast<uint32_t>(STRATA_FLAG_VSYNC);
    desc.title = "Old Town Walk";
    StrataEngine* engine = nullptr;
    if (!check(strata_create(&desc, &engine), "cannot start the renderer")) return 1;
    std::printf("Strata %s on %s\n", strata_version_string(), strata_gpu_description(engine));

    const double startEast = manifest.startEast + a.east, startNorth = manifest.startNorth + a.north;
    const otw::TileKey origin{static_cast<int>(std::floor(startEast / manifest.tileSize)),
                                static_cast<int>(std::floor(startNorth / manifest.tileSize))};
    otw::StreamConfig config;
    config.loadRadius = a.radius;
    config.unloadRadius = a.radius + 200;
    Stream stream(engine, manifest, origin, config);

    otw::Walker walker;
    walker.x = static_cast<float>(startEast - origin.ix * static_cast<double>(manifest.tileSize));
    walker.z = static_cast<float>((origin.iy + 1) * static_cast<double>(manifest.tileSize) - startNorth);
    walker.yawDeg = a.yaw;
    walker.pitchDeg = a.pitch;
    bool ok = stream.update(walker.x, walker.z, 1000);
    if (ok) {
        // A start point inside a building moves to open ground.
        for (float r = 0; r < 60 && stream.solids().inside(walker.x, walker.z); r += 1.0f) walker.x += 1.0f;
        walker.eyeY = stream.groundAt(walker.x, walker.z) + otw::kEyeHeight;
    }

    StrataLighting light{};
    strata_lighting_defaults(STRATA_LIGHTING_SUN, &light);
    light.latitude_degrees = static_cast<float>(manifest.latitude);
    light.time_of_day = a.time;
    light.cascade_count = 4;
    light.shadow_map_size = 2048;
    light.shadow_distance = 300.0f;
    light.split_lambda = 0.8f;
    light.fog = 1;

    int w = a.width, h = a.height;
    bool showLabel = true;
    auto draw = [&]() -> bool {
        float ahead[3];
        walker.forward(ahead);
        const float eye[3] = {walker.x, walker.eyeY, walker.z};
        const float look[3] = {eye[0] + ahead[0], eye[1] + ahead[1], eye[2] + ahead[2]};
        const float up[3] = {0, 1, 0};
        StrataCamera c{};
        strata_camera_look_at(eye, look, up, 70.0f, static_cast<float>(w) / static_cast<float>(std::max(1, h)), 0.1f, 1600.0f, &c);
        StrataFrameDesc fd{};
        fd.clear_flags = STRATA_CLEAR_COLOR | STRATA_CLEAR_DEPTH | STRATA_CLEAR_SKY;
        const auto& trees = stream.trees();
        StrataText label[3]{};
        const char* lines[3] = {"Old Town Walk: Fort Collins, Colorado",
                                "WASD walk, Shift run, F fly, mouse look, Esc mouse, H hide",
                                "Map data (c) OpenStreetMap contributors, ODbL. Elevation: USGS."};
        for (int i = 0; i < 3; ++i) {
            label[i].x = 12;
            label[i].y = 12.0f + 22.0f * static_cast<float>(i);
            label[i].pixel_size = i == 0 ? 18 : 14;
            label[i].flags = STRATA_TEXT_SHADOW;
            label[i].color[0] = label[i].color[1] = label[i].color[2] = label[i].color[3] = 1.0f;
            label[i].utf8 = lines[i];
        }
        return check(strata_set_camera(engine, &c), "camera") && check(strata_set_lighting(engine, &light), "lighting") &&
               check(strata_begin_frame(engine, &fd), "begin_frame") &&
               check(strata_submit_primitives(engine, trees.data(), static_cast<int32_t>(trees.size())), "trees") &&
               (a.headless || !showLabel || check(strata_submit_text(engine, label, 3), "label")) &&
               check(strata_end_frame(engine), "end_frame");
    };
    bool flying = a.fly;
    auto walkableHere = [&](float x, float z) { return stream.walkable(x, z); };
    auto groundHere = [&](float x, float z) { return stream.groundAt(x, z); };
    if (flying) walker.eyeY = stream.groundAt(walker.x, walker.z) + a.altitude;
    auto toggleFlight = [&] {
        flying = !flying;
        if (flying) walker.eyeY += otw::kFlyClearance;  // lift off at once
        else walker.clearOfBuildings(stream.solids(), walkableHere);
        std::puts(flying ? "flying" : "on foot");
    };
    auto stepWalker = [&](float ahead, float right, bool run, float dt, float up = 0) {
        if (flying) {
            walker.fly(walkableHere, groundHere, ahead, right, up, run, dt);
            return;
        }
        walker.step(stream.solids(), [&](float x, float z) { return stream.walkable(x, z); }, ahead, right, run, dt);
        const float target = stream.groundAt(walker.x, walker.z) + otw::kEyeHeight;
        walker.eyeY += (target - walker.eyeY) * std::min(1.0f, dt * 10.0f);
    };

    if (a.headless) {
        const float dt = 1.0f / 60.0f;
        for (int i = 0, steps = static_cast<int>(a.walkAhead * 60.0f); ok && i < steps; ++i) {
            stepWalker(1, 0, a.run, dt);
            ok = stream.update(walker.x, walker.z, 1);
        }
        const float passLength = std::hypot(a.passEast, a.passNorth);
        if (ok && passLength > 0) {
            const int steps = static_cast<int>(passLength * 2);  // half a metre at a time, a tile at most per step
            const float x0 = walker.x, z0 = walker.z;
            for (int i = 1; ok && i <= steps; ++i) {
                const float u = static_cast<float>(i) / static_cast<float>(steps);
                walker.x = x0 + a.passEast * u;
                walker.z = z0 - a.passNorth * u;
                ok = stream.update(walker.x, walker.z, 1);
            }
            walker.eyeY = stream.groundAt(walker.x, walker.z) + otw::kEyeHeight;
            if (stream.unloads() == 0 || stream.peakMeshes() > STRATA_MAX_MESHES) {
                std::fprintf(stderr, "old-town-walk: the pass did not stream (%d unloads, peak %d meshes)\n", stream.unloads(), stream.peakMeshes());
                ok = false;
            }
        }
        if (ok && a.bench > 0) {
            const auto t0 = Clock::now();
            for (int i = 0; ok && i < a.bench; ++i) {
                walker.look(360.0f / static_cast<float>(a.bench), 0);
                ok = draw();
            }
            std::printf("bench: %.1f ms a frame over %d frames, a full turn\n", ms(t0) / a.bench, a.bench);
        }
        if (ok && !a.screenshot.empty()) ok = draw() && check(strata_save_screenshot_png(engine, a.screenshot.c_str()), "screenshot");
        StrataFrameStats st{};
        strata_get_frame_stats(engine, &st);
        std::printf("at %.1f, %.1f (tile %d %d); %d tiles held, %d meshes, peak %d of %d; %d loads, %d unloads, read %.1f ms, upload "
                    "%.1f ms each, worst %.1f ms; %d draw calls\n",
                    static_cast<double>(walker.x), static_cast<double>(walker.z), stream.keyAt(walker.x, walker.z).ix,
                    stream.keyAt(walker.x, walker.z).iy, stream.tileCount(), stream.meshes(), stream.peakMeshes(),
                    STRATA_MAX_MESHES, stream.loads(), stream.unloads(), stream.readMs() / std::max(1, stream.loads()),
                    stream.uploadMs() / std::max(1, stream.loads()), stream.worstLoadMs(), st.draw_calls);
        strata_destroy(engine);
        return ok ? 0 : 1;
    }

    std::puts("WASD walk, Shift run, F fly (E up, Q down), mouse or arrows look, [ ] time, P screenshot, Esc frees the mouse, Esc again quits");
    bool running = ok, mouseHeld = false;
    auto holdMouse = [&](bool hold) { mouseHeld = check(strata_set_relative_mouse_mode(engine, hold ? 1 : 0), "mouse look") && hold; };
    holdMouse(true);
    bool held[256] = {};
    int shots = 0;
    auto last = Clock::now();
    while (running) {
        StrataEvent e;
        while (strata_poll_event(engine, &e)) {
            switch (e.type) {
            case STRATA_EVENT_QUIT: running = false; break;
            case STRATA_EVENT_RESIZED: strata_get_drawable_size(engine, &w, &h); break;
            case STRATA_EVENT_KEY_DOWN:
                if (e.scancode >= 0 && e.scancode < 256) held[e.scancode] = true;
                if (e.repeat) break;
                if (e.scancode == kEscape) {
                    if (mouseHeld) holdMouse(false);
                    else running = false;
                } else if (e.scancode == kF) {
                    toggleFlight();
                } else if (e.scancode == kH) {
                    showLabel = !showLabel;
                } else if (e.scancode == kP) {
                    char name[64];
                    std::snprintf(name, sizeof name, "old-town-walk-%03d.png", shots++);
                    if (check(strata_save_screenshot_png(engine, name), "screenshot")) std::printf("saved %s\n", name);
                }
                break;
            case STRATA_EVENT_KEY_UP:
                if (e.scancode >= 0 && e.scancode < 256) held[e.scancode] = false;
                break;
            case STRATA_EVENT_MOUSE_BUTTON_DOWN:
                if (!mouseHeld) holdMouse(true);
                break;
            case STRATA_EVENT_MOUSE_MOTION:
                if (mouseHeld) walker.look(e.dx * 0.15f, -e.dy * 0.15f);
                break;
            default: break;
            }
        }
        const auto now = Clock::now();
        const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        last = now;
        walker.look(((held[kRight] ? 1.0f : 0.0f) - (held[kLeft] ? 1.0f : 0.0f)) * 90.0f * dt,
                    ((held[kUp] ? 1.0f : 0.0f) - (held[kDown] ? 1.0f : 0.0f)) * 60.0f * dt);
        stepWalker((held[kW] ? 1.0f : 0.0f) - (held[kS] ? 1.0f : 0.0f), (held[kD] ? 1.0f : 0.0f) - (held[kA] ? 1.0f : 0.0f),
                   held[225] || held[229], dt,
                   (held[kE] || held[kSpace] ? 1.0f : 0.0f) - (held[kQ] || held[kLeftCtrl] ? 1.0f : 0.0f));
        if (held[kLeftBracket]) light.time_of_day -= dt * 0.05f;
        if (held[kRightBracket]) light.time_of_day += dt * 0.05f;
        light.time_of_day -= std::floor(light.time_of_day);
        if (!stream.update(walker.x, walker.z, 1) || !draw() || !check(strata_present(engine), "present")) running = false;
    }
    strata_destroy(engine);
    return 0;
}
