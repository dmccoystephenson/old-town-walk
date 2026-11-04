// Which tiles should be in memory for a viewer at a position. Pure: tested without a GPU.
#pragma once

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "tile.h"

namespace otw {

struct StreamConfig {
    float loadRadius = 700;    // a tile this close to the viewer gets loaded
    float unloadRadius = 900;  // and stays until it is this far, so walking along an edge does not thrash
    int maxTiles = 60;         // four meshes a tile, and Strata holds STRATA_MAX_MESHES (256)
};

struct StreamPlan {
    std::vector<TileKey> load;    // nearest first
    std::vector<TileKey> unload;
};

// Distance from a point to the nearest edge of a tile; zero inside it.
inline double tileDistance(TileKey k, double east, double north, double size) {
    const double west = k.ix * size, south = k.iy * size;
    const double dx = std::max({west - east, 0.0, east - (west + size)});
    const double dy = std::max({south - north, 0.0, north - (south + size)});
    return std::hypot(dx, dy);
}

inline StreamPlan planStreaming(const std::vector<TileKey>& available, const std::set<TileKey>& loaded, double east,
                                double north, double size, const StreamConfig& c) {
    StreamPlan plan;
    for (const TileKey& k : loaded) {
        if (tileDistance(k, east, north, size) > c.unloadRadius) plan.unload.push_back(k);
    }
    std::vector<std::pair<double, TileKey>> wanted;
    for (const TileKey& k : available) {
        const double d = tileDistance(k, east, north, size);
        if (d <= c.loadRadius && loaded.count(k) == 0) wanted.push_back({d, k});
    }
    std::sort(wanted.begin(), wanted.end());
    int room = c.maxTiles - (static_cast<int>(loaded.size()) - static_cast<int>(plan.unload.size()));
    for (const auto& w : wanted) {
        if (room-- <= 0) break;
        plan.load.push_back(w.second);
    }
    return plan;
}

}  // namespace otw
