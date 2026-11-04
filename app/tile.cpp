#include "tile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

namespace otw {
namespace {

struct Reader {
    std::FILE* f;
    bool ok = true;
    template <typename T>
    T get() {
        T v{};
        if (ok && std::fread(&v, sizeof v, 1, f) != 1) ok = false;
        return v;
    }
    template <typename T>
    void array(std::vector<T>& v, size_t n) {
        v.resize(n);
        if (ok && n && std::fread(v.data(), sizeof(T), n, f) != n) ok = false;
    }
    void pad(size_t bytes) {
        char skip[4];
        const size_t n = (4 - bytes % 4) % 4;
        if (ok && n && std::fread(skip, 1, n, f) != n) ok = false;
    }
};

constexpr int kMaxMeshes = 16, kMaxVertices = 1 << 24, kMaxIndices = 1 << 26, kMaxCount = 1000000;

}  // namespace

float Tile::heightAt(float x, float z) const {
    const float fx = std::clamp(x / cell, 0.0f, static_cast<float>(samples - 1));
    const float fz = std::clamp(z / cell, 0.0f, static_cast<float>(samples - 1));
    const int c = std::min(static_cast<int>(fx), samples - 2), r = std::min(static_cast<int>(fz), samples - 2);
    const float u = fx - static_cast<float>(c), v = fz - static_cast<float>(r);
    const float north = height(c, r) + (height(c + 1, r) - height(c, r)) * u;
    const float south = height(c, r + 1) + (height(c + 1, r + 1) - height(c, r + 1)) * u;
    return north + (south - north) * v;
}

bool Manifest::has(TileKey k) const { return std::find(tiles.begin(), tiles.end(), k) != tiles.end(); }

std::string Manifest::path(TileKey k) const {
    return dir + "/" + std::to_string(k.ix) + "_" + std::to_string(k.iy) + ".tile";
}

bool loadManifest(const std::string& dir, Manifest& m, std::string* error) {
    std::ifstream in(dir + "/manifest.txt");
    if (!in) {
        if (error) *error = "cannot open " + dir + "/manifest.txt (run `uv run old-town-walk build` first)";
        return false;
    }
    m = Manifest{};
    m.dir = dir;
    std::string line, word;
    bool header = false;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        s >> word;
        if (word == "old-town-walk-manifest") {
            int version = 0;
            s >> version;
            header = version == 1;
        } else if (word == "name") s >> m.name;
        else if (word == "tile_size") s >> m.tileSize;
        else if (word == "cell") s >> m.cell;
        else if (word == "datum") s >> m.datum;
        else if (word == "latitude") s >> m.latitude;
        else if (word == "start") s >> m.startEast >> m.startNorth;
        else if (word == "tile") {
            TileKey k;
            s >> k.ix >> k.iy;
            if (s) m.tiles.push_back(k);
        }
    }
    if (!header || m.tileSize <= 0 || m.tiles.empty()) {
        if (error) *error = dir + "/manifest.txt is not a manifest this viewer reads";
        return false;
    }
    return true;
}

bool loadTile(const std::string& path, Tile& t, std::string* error) {
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path.c_str(), "rb"), &std::fclose);
    auto fail = [&](const char* what) {
        if (error) *error = path + ": " + what;
        return false;
    };
    if (!file) return fail("cannot open");
    Reader r{file.get()};
    char magic[8];
    if (std::fread(magic, 1, 8, file.get()) != 8 || std::memcmp(magic, "OTWTILE1", 8) != 0) return fail("not a tile");
    t = Tile{};
    if (r.get<uint32_t>() != 1) return fail("unsupported version");
    t.key.ix = r.get<int32_t>();
    t.key.iy = r.get<int32_t>();
    t.size = r.get<float>();
    t.cell = r.get<float>();
    t.samples = r.get<int32_t>();
    const auto meshCount = r.get<int32_t>();
    const auto footprintCount = r.get<int32_t>();
    const auto treeCount = r.get<int32_t>();
    if (!r.ok || t.samples < 2 || t.samples > 2049 || meshCount < 0 || meshCount > kMaxMeshes || footprintCount < 0 ||
        footprintCount > kMaxCount || treeCount < 0 || treeCount > kMaxCount)
        return fail("bad header");
    const size_t ring = static_cast<size_t>(t.samples) + 2, n = static_cast<size_t>(t.samples);
    r.array(t.heights, ring * ring);
    r.array(t.colors, n * n * 3);
    r.pad(n * n * 3);
    t.meshes.resize(static_cast<size_t>(meshCount));
    for (TileMesh& m : t.meshes) {
        m.kind = r.get<uint32_t>();
        const auto vc = r.get<int32_t>();
        const auto ic = r.get<int32_t>();
        if (!r.ok || vc <= 0 || ic <= 0 || vc > kMaxVertices || ic > kMaxIndices) return fail("bad mesh header");
        const size_t verts = static_cast<size_t>(vc), count = static_cast<size_t>(ic);
        r.array(m.positions, verts * 3);
        r.array(m.normals, verts * 4);
        r.array(m.colors, verts * 4);
        if (vc < 65536) {
            std::vector<uint16_t> small;
            r.array(small, count);
            r.pad(count * 2);
            m.indices.assign(small.begin(), small.end());
        } else {
            r.array(m.indices, count);
        }
        if (!r.ok) return fail("truncated mesh");
        for (uint32_t i : m.indices) {
            if (i >= static_cast<uint32_t>(vc)) return fail("mesh index out of range");
        }
    }
    t.footprints.resize(static_cast<size_t>(footprintCount));
    for (Footprint& f : t.footprints) {
        const auto points = r.get<int32_t>();
        if (!r.ok || points < 3 || points > (1 << 20)) return fail("bad footprint");
        r.array(f.xz, static_cast<size_t>(points) * 2);
    }
    t.trees.resize(static_cast<size_t>(treeCount));
    for (Tree& tree : t.trees) tree = r.get<Tree>();
    return r.ok || fail("truncated");
}

}  // namespace otw
