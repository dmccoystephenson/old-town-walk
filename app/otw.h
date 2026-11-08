// Old Town Walk on streetkit: the game's own names (otw::Settings and the rest of settings.h) live beside the
// library's, so otw::Walker, otw::Stream and friends are streetkit's.
#pragma once

namespace streetkit {}
namespace otw {
using namespace streetkit;
}

namespace otw {
// Where every walk begins, in metres from the map's centre: the open crossing at the north-east end of Old Town
// Square, facing south-west down the square to the bandstand. tests/python/test_start.py keeps it clear of buildings.
constexpr double kStartEast = 64, kStartNorth = 41;
constexpr float kStartYaw = 225;
}  // namespace otw
