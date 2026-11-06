// What the player can change while playing. Pure: tested without a GPU.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace otw {

constexpr float kMinFlySpeed = 5.0f, kMaxFlySpeed = 800.0f;    // metres a second
constexpr float kMinWalkSpeed = 0.7f, kMaxWalkSpeed = 14.0f;
constexpr float kMinBikeSpeed = 3.0f, kMaxBikeSpeed = 40.0f;
constexpr float kMinSensitivity = 0.03f, kMaxSensitivity = 0.6f;   // degrees a pixel of mouse movement
constexpr float kMinFov = 40.0f, kMaxFov = 110.0f, kFovStep = 5.0f;  // vertical, in degrees
constexpr float kSpeedNotch = 1.25f;                           // one wheel notch
constexpr float kMinViewRadius = 200.0f, kMaxViewRadius = 1500.0f, kViewRadiusStep = 100.0f;  // 1500 m fits the tile budget

// A notch multiplies, so it feels the same at a stroll as at full speed.
inline float stepSpeed(float speed, int notches, float lowest, float highest) {
    return std::clamp(speed * std::pow(kSpeedNotch, static_cast<float>(notches)), lowest, highest);
}

inline float stepViewRadius(float radius, int steps) {
    const float snapped = std::round(radius / kViewRadiusStep) + static_cast<float>(steps);
    return std::clamp(snapped * kViewRadiusStep, kMinViewRadius, kMaxViewRadius);
}

constexpr const char* kHelpText =
    "WASD       move\n"
    "mouse      look (arrows too)\n"
    "Shift      go faster\n"
    "wheel      slower / faster\n"
    "- =        shorter / longer view\n"
    ", .        mouse sensitivity\n"
    "9 0        field of view\n"
    "I          invert mouse\n"
    "Z          hold to zoom\n"
    "Space      jump\n"
    "F          fly / land\n"
    "B          get on / off the bike\n"
    "E Q        up / down\n"
    "[ ]        time of day\n"
    "R          back to the start\n"
    "P          screenshot\n"
    "Tab        hide the panel\n"
    "Backspace  default settings\n"
    "H          hide this help\n"
    "Esc        free the mouse";

// What is kept from one run to the next.
struct Settings {
    float flySpeed = 20, walkSpeed = 1.4f, viewRadius = 700, sensitivity = 0.15f, fov = 70;
    bool panel = true, invertLook = false;
    float bikeSpeed = 12.0f;
};

// One "name value" a line. Anything unknown or unreadable keeps its default, and values are held to their ranges, so
// a hand-edited or older file cannot leave the viewer in a bad state.
inline Settings parseSettings(const std::string& text) {
    Settings s;
    std::istringstream lines(text);
    std::string name;
    float value;
    while (lines >> name) {
        if (!(lines >> value)) {
            lines.clear();
            lines.ignore(1000, '\n');
            continue;
        }
        if (!std::isfinite(value)) continue;
        if (name == "fly_speed") s.flySpeed = std::clamp(value, kMinFlySpeed, kMaxFlySpeed);
        else if (name == "walk_speed") s.walkSpeed = std::clamp(value, kMinWalkSpeed, kMaxWalkSpeed);
        else if (name == "view_radius") s.viewRadius = std::clamp(value, kMinViewRadius, kMaxViewRadius);
        else if (name == "sensitivity") s.sensitivity = std::clamp(value, kMinSensitivity, kMaxSensitivity);
        else if (name == "fov") s.fov = std::clamp(value, kMinFov, kMaxFov);
        else if (name == "panel") s.panel = value != 0;
        else if (name == "invert_look") s.invertLook = value != 0;
        else if (name == "bike_speed") s.bikeSpeed = std::clamp(value, kMinBikeSpeed, kMaxBikeSpeed);
    }
    return s;
}

inline std::string formatSettings(const Settings& s) {
    char text[240];
    std::snprintf(text, sizeof text, "fly_speed %.2f\nwalk_speed %.2f\nview_radius %.0f\nsensitivity %.3f\nfov %.0f\npanel %d\ninvert_look %d\n\nbike_speed %.2f",
                  static_cast<double>(s.flySpeed), static_cast<double>(s.walkSpeed), static_cast<double>(s.viewRadius),
                  static_cast<double>(s.sensitivity), static_cast<double>(s.fov), s.panel ? 1 : 0, s.invertLook ? 1 : 0, static_cast<double>(s.bikeSpeed));
    return text;
}

// Seconds of movement a frame gets. Long enough to keep the speed honest at 4 frames a second, and short enough
// that a stall loading tiles cannot throw the player across the map.
inline float frameStep(float elapsed) { return std::min(elapsed, 0.25f); }

inline float stepFov(float fov, int steps) {
    return std::clamp(fov + static_cast<float>(steps) * kFovStep, kMinFov, kMaxFov);
}

// Yaw 0 faces north, 90 east.
inline const char* compass(float yawDeg) {
    static const char* const names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const float turns = yawDeg / 45.0f + 0.5f;
    return names[static_cast<int>(std::floor(turns)) & 7];
}

struct HudState {
    bool flying;
    float speed, viewRadius, height, yawDeg, fps;
    int tiles, loading;
    float dayFraction;  // 0 midnight, 0.5 noon
    std::string street = {};  // empty away from any named road
    bool riding = false;
    bool running = false;
};

// The status panel: what mode you are in and what your settings are, one line each.
inline std::string hudText(const HudState& s) {
    char more[24] = "";
    if (s.loading > 0) std::snprintf(more, sizeof more, " +%d loading", s.loading);
    const int minutes = static_cast<int>(s.dayFraction * 1440.0f) % 1440;
    char text[260];
    std::snprintf(text, sizeof text, "%s  %.*f m/s\nstreet  %s\nheight  %.0f m\nview    %.0f m\nfacing  %s\nclock   %02d:%02d\ntiles   %d%s\n%.0f fps  H help",
                  s.riding ? "riding" : s.flying ? "flying" : s.running ? "running" : "on foot", s.speed < 10 ? 1 : 0, static_cast<double>(s.speed), s.street.empty() ? "-" : s.street.c_str(), static_cast<double>(s.height),
                  static_cast<double>(s.viewRadius), compass(s.yawDeg), minutes / 60, minutes % 60, s.tiles, more, static_cast<double>(s.fps));
    return text;
}

}  // namespace otw
