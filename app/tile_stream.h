// The tiles in memory and on the GPU, shared by the desktop viewer and the browser page.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "streaming.h"
#include "strata/strata.h"
#include "tile.h"
#include "walker.h"

namespace otw {

using Clock = std::chrono::steady_clock;
inline double msSince(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }

// Where a tile comes from: a file on the desktop, a download in a browser. Pending means not yet: the
// stream asks again on a later update.
enum class TileFetch { Ready, Pending, Failed };
using TileSource = std::function<TileFetch(TileKey, Tile&, std::string*)>;

inline bool check(StrataStatus s, const char* what) {
    if (s == STRATA_OK) return true;
    std::fprintf(stderr, "old-town-walk: %s: %s (%s)\n", what, strata_status_string(s), strata_last_error());
    return false;
}

inline float linearOf(uint8_t srgb) { return std::pow(static_cast<float>(srgb) / 255.0f, 2.2f); }

// The tiles in memory and on the GPU. World x, z are metres from the north-west corner of the start tile.
class Stream {
public:
    Stream(StrataEngine* engine, const Manifest& m, TileKey origin, StreamConfig c, TileSource source)
        : engine_(engine), m_(m), origin_(origin), config_(c), source_(std::move(source)) {
        for (int i = 0; i < 256; ++i) linear_[i] = linearOf(static_cast<uint8_t>(i));
    }

    float offsetX(TileKey k) const { return static_cast<float>(k.ix - origin_.ix) * m_.tileSize; }
    float offsetZ(TileKey k) const { return static_cast<float>(origin_.iy - k.iy) * m_.tileSize; }
    double east(float x) const { return origin_.ix * static_cast<double>(m_.tileSize) + x; }
    double north(float z) const { return (origin_.iy + 1) * static_cast<double>(m_.tileSize) - z; }
    TileKey keyAt(float x, float z) const {
        return {static_cast<int>(std::floor(east(x) / m_.tileSize)), static_cast<int>(std::floor(north(z) / m_.tileSize))};
    }

    // Applies the plan for a position, loading at most maxLoads tiles.
    bool update(float x, float z, int maxLoads) {
        std::set<TileKey> loaded;
        for (const auto& t : tiles_) loaded.insert(t.first);
        const StreamPlan plan =
            planStreaming(m_.tiles, loaded, east(x), north(z), m_.tileSize, config_);
        for (const TileKey& k : plan.unload) drop(k);
        int count = 0;
        waiting_ = static_cast<int>(plan.load.size());
        for (const TileKey& k : plan.load) {
            if (count >= maxLoads) break;
            const TileFetch got = load(k);
            if (got == TileFetch::Failed) return false;
            if (got == TileFetch::Ready) {
                ++count;
                --waiting_;
            }
        }
        return true;
    }

    // After the GPU was lost: everything on it is gone, so the tiles are fetched and uploaded again.
    void restart(StrataEngine* engine) {
        engine_ = engine;
        tiles_.clear();
        solids_ = Solids{};
        trees_.clear();
        meshCount_ = 0;
    }
    void setLoadRadius(float radius) {
        config_.loadRadius = radius;
        config_.unloadRadius = radius + 200;
    }

    bool walkable(float x, float z) const { return tiles_.count(keyAt(x, z)) != 0; }
    float groundAt(float x, float z) const {
        const TileKey k = keyAt(x, z);
        const auto t = tiles_.find(k);
        if (t == tiles_.end()) return 0;
        return t->second.tile.heightAt(x - offsetX(k), z - offsetZ(k));
    }
    const Solids& solids() const { return solids_; }
    const std::vector<StrataRenderItem>& trees() const { return trees_; }
    int meshes() const { return meshCount_; }
    int peakMeshes() const { return peakMeshes_; }
    int tileCount() const { return static_cast<int>(tiles_.size()); }
    int waiting() const { return waiting_; }
    float viewRadius() const { return config_.loadRadius; }
    bool overTile(float x, float z) const { const TileKey k = keyAt(x, z); return std::find(m_.tiles.begin(), m_.tiles.end(), k) != m_.tiles.end(); }
    int loads() const { return loads_; }
    int unloads() const { return unloads_; }
    double readMs() const { return readMs_; }
    double uploadMs() const { return uploadMs_; }
    double worstLoadMs() const { return worstLoadMs_; }

private:
    struct Loaded {
        Tile tile;
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
        const Tile& t = l.tile;
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

    TileFetch load(TileKey k) {
        const auto t0 = Clock::now();
        Loaded l;
        std::string err;
        const TileFetch got = source_(k, l.tile, &err);
        if (got == TileFetch::Failed) std::fprintf(stderr, "old-town-walk: %s\n", err.c_str());
        if (got != TileFetch::Ready) return got;
        const double read = msSince(t0);
        const auto t1 = Clock::now();
        const float ox = offsetX(k), oz = offsetZ(k);
        if (!addTerrain(l, ox, oz)) return TileFetch::Failed;
        for (const TileMesh& m : l.tile.meshes) {
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
            if (!add(mesh, l)) return TileFetch::Failed;
        }
        solids_.add(l.tile, ox, oz);
        // The GPU has these now; heights, outlines and trees are what stays useful.
        std::vector<TileMesh>().swap(l.tile.meshes);
        std::vector<uint8_t>().swap(l.tile.colors);
        std::vector<Footprint>().swap(l.tile.footprints);
        tiles_[k] = std::move(l);
        rebuildTrees();
        const double upload = msSince(t1);
        readMs_ += read;
        uploadMs_ += upload;
        worstLoadMs_ = std::max(worstLoadMs_, read + upload);
        ++loads_;
        return TileFetch::Ready;
    }

    void drop(TileKey k) {
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
            for (const Tree& t : entry.second.tile.trees) {
                const float trunk = t.height * 0.45f, crown = t.height * 0.7f;
                trees_.push_back({STRATA_PRIMITIVE_CYLINDER, {t.x + ox, t.y + trunk * 0.5f, t.z + oz},
                                  {kTrunkRadius, trunk * 0.5f, kTrunkRadius}, {0.105f, 0.065f, 0.034f, 1}});
                trees_.push_back({STRATA_PRIMITIVE_SPHERE, {t.x + ox, t.y + t.height - crown * 0.5f, t.z + oz},
                                  {t.crown, crown, t.crown}, {t.r, t.g, t.b, 1}});
            }
        }
    }

    StrataEngine* engine_;
    const Manifest& m_;
    TileKey origin_;
    StreamConfig config_;
    TileSource source_;
    float linear_[256];
    std::map<TileKey, Loaded> tiles_;
    Solids solids_;
    std::vector<StrataRenderItem> trees_;
    int waiting_ = 0, meshCount_ = 0, peakMeshes_ = 0, loads_ = 0, unloads_ = 0;
    double readMs_ = 0, uploadMs_ = 0, worstLoadMs_ = 0;
};


}  // namespace otw
