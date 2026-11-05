// A person on the ground. Everything solid is kept per tile, so a tile takes its walls with it when it unloads.
#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "tile.h"

namespace otw {

constexpr float kEyeHeight = 1.65f, kBodyRadius = 0.3f, kWalkSpeed = 1.4f, kRunSpeed = 5.0f, kTrunkRadius = 0.35f;
constexpr float kFlySpeed = 20.0f, kFlyFastSpeed = 80.0f;
constexpr float kJumpSpeed = 4.7f, kGravity = 9.8f;  // leaves the ground at this speed, for a hop of about 1.1 m
constexpr float kFlyClearance = 2.0f, kFlyCeiling = 400.0f;  // metres above the ground

struct SolidOutline {
    std::vector<float> xz;  // world x, z
    float minX, minZ, maxX, maxZ;
};
struct SolidTrunk {
    float x, z;
};

class Solids {
public:
    void add(const Tile& t, float offsetX, float offsetZ) {
        Set& s = tiles_[t.key];
        for (const Footprint& f : t.footprints) {
            SolidOutline o{f.xz, 1e30f, 1e30f, -1e30f, -1e30f};
            for (size_t i = 0; i + 1 < o.xz.size(); i += 2) {
                o.xz[i] += offsetX;
                o.xz[i + 1] += offsetZ;
                o.minX = std::min(o.minX, o.xz[i]);
                o.maxX = std::max(o.maxX, o.xz[i]);
                o.minZ = std::min(o.minZ, o.xz[i + 1]);
                o.maxZ = std::max(o.maxZ, o.xz[i + 1]);
            }
            s.outlines.push_back(std::move(o));
        }
        for (const Tree& tree : t.trees) s.trunks.push_back({tree.x + offsetX, tree.z + offsetZ});
    }
    void remove(TileKey k) { tiles_.erase(k); }

    bool inside(float x, float z) const {
        for (const auto& t : tiles_) {
            for (const SolidOutline& o : t.second.outlines) {
                if (x < o.minX || x > o.maxX || z < o.minZ || z > o.maxZ) continue;
                bool in = false;
                const size_t n = o.xz.size() / 2;
                for (size_t i = 0, j = n - 1; i < n; j = i++) {
                    const float xi = o.xz[i * 2], zi = o.xz[i * 2 + 1], xj = o.xz[j * 2], zj = o.xz[j * 2 + 1];
                    if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) in = !in;
                }
                if (in) return true;
            }
        }
        return false;
    }

    void pushOut(float& x, float& z) const {
        for (const auto& t : tiles_) {
            for (const SolidOutline& o : t.second.outlines) {
                if (x < o.minX - kBodyRadius || x > o.maxX + kBodyRadius || z < o.minZ - kBodyRadius || z > o.maxZ + kBodyRadius)
                    continue;
                const size_t n = o.xz.size() / 2;
                for (size_t i = 0, j = n - 1; i < n; j = i++) {
                    const float ax = o.xz[j * 2], az = o.xz[j * 2 + 1], ex = o.xz[i * 2] - ax, ez = o.xz[i * 2 + 1] - az;
                    const float len2 = ex * ex + ez * ez;
                    const float u = len2 > 0 ? std::clamp(((x - ax) * ex + (z - az) * ez) / len2, 0.0f, 1.0f) : 0.0f;
                    clear(x, z, ax + ex * u, az + ez * u, kBodyRadius);
                }
            }
            for (const SolidTrunk& tree : t.second.trunks) clear(x, z, tree.x, tree.z, kBodyRadius + kTrunkRadius);
        }
    }

private:
    struct Set {
        std::vector<SolidOutline> outlines;
        std::vector<SolidTrunk> trunks;
    };
    static void clear(float& x, float& z, float px, float pz, float distance) {
        const float dx = x - px, dz = z - pz, d = std::hypot(dx, dz);
        if (d >= distance || d < 1e-5f) return;
        x = px + dx / d * distance;
        z = pz + dz / d * distance;
    }
    std::map<TileKey, Set> tiles_;
};

// Yaw 0 faces north (-z), 90 faces east (+x).
struct Walker {
    float x = 0, z = 0, yawDeg = 0, pitchDeg = 0, eyeY = 0;
    float hop = 0, hopSpeed = 0;  // height of a jump above the ground, kept apart from eyeY, which eases over the terrain

    // A jump starts only from the ground, so holding the key hops rather than climbs.
    void jump() {
        if (hop <= 0) hopSpeed = kJumpSpeed;
    }
    void stepHop(float dt) {
        if (hop <= 0 && hopSpeed <= 0) return;
        hop += hopSpeed * dt;
        hopSpeed -= kGravity * dt;
        if (hop <= 0) hop = hopSpeed = 0;
    }

    void look(float dyaw, float dpitch) {
        yawDeg = std::fmod(yawDeg + dyaw + 360.0f, 360.0f);
        pitchDeg = std::clamp(pitchDeg + dpitch, -85.0f, 85.0f);
    }
    void forward(float out[3]) const {
        const float yaw = yawDeg * 3.14159265f / 180.0f, pitch = pitchDeg * 3.14159265f / 180.0f;
        out[0] = std::sin(yaw) * std::cos(pitch);
        out[1] = std::sin(pitch);
        out[2] = -std::cos(yaw) * std::cos(pitch);
    }
    // Moves by ahead/right (-1..1), sliding along solids and stopping where `walkable(x, z)` says no.
    template <typename Walkable>
    void step(const Solids& solids, Walkable&& walkable, float ahead, float right, bool run, float dt, float speed = kWalkSpeed) {
        const float length = std::hypot(ahead, right);
        if (length <= 0) return;
        const float yaw = yawDeg * 3.14159265f / 180.0f, scale = (run ? speed * kRunSpeed / kWalkSpeed : speed) * dt / std::max(1.0f, length);
        const float fx = std::sin(yaw), fz = -std::cos(yaw);
        const float dx = (ahead * fx - right * fz) * scale, dz = (ahead * fz + right * fx) * scale;
        const int steps = std::max(1, static_cast<int>(std::ceil(std::hypot(dx, dz) / (kBodyRadius * 0.5f))));
        for (int i = 0; i < steps; ++i) {
            float nx = x + dx / static_cast<float>(steps), nz = z + dz / static_cast<float>(steps);
            for (int pass = 0; pass < 3; ++pass) solids.pushOut(nx, nz);
            if (walkable(nx, nz)) {
                x = nx;
                z = nz;
            }
        }
    }
    // Moves freely toward the view: ahead and right follow the look, up follows the world. Solids do not
    // stop a flight, but `walkable(x, z)` does, one axis at a time so the edge of the map can be slid along.
    template <typename Walkable, typename Ground>
    void fly(Walkable&& walkable, Ground&& ground, float ahead, float right, float up, bool fast, float dt, float speed = kFlySpeed) {
        const float length = std::sqrt(ahead * ahead + right * right + up * up);
        if (length > 0) {
            float f[3];
            forward(f);
            const float yaw = yawDeg * 3.14159265f / 180.0f, scale = (fast ? speed * kFlyFastSpeed / kFlySpeed : speed) * dt / std::max(1.0f, length);
            const float dx = (ahead * f[0] + right * std::cos(yaw)) * scale, dz = (ahead * f[2] + right * std::sin(yaw)) * scale;
            if (walkable(x + dx, z)) x += dx;
            if (walkable(x, z + dz)) z += dz;
            eyeY += (ahead * f[1] + up) * scale;
        }
        const float floor = ground(x, z);
        eyeY = std::clamp(eyeY, floor + kFlyClearance, floor + kFlyCeiling);
    }
    // Steps sideways out of any building, for landing on a roof.
    template <typename Walkable>
    void clearOfBuildings(const Solids& solids, Walkable&& walkable) {
        for (float r = 1; r <= 80 && solids.inside(x, z); r += 1) {
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i) * 0.7854f, nx = x + r * std::cos(a), nz = z + r * std::sin(a);
                if (walkable(nx, nz) && !solids.inside(nx, nz)) {
                    x = nx;
                    z = nz;
                    return;
                }
            }
        }
    }
};

}  // namespace otw
