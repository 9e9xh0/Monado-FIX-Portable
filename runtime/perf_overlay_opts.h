// [XCDE] GitHub #147: size and corner of the performance overlay (runtime/ui/perf_overlay.h), shared by the game
// (xcde.ini, F1 menu) and the launcher. Header-only, C and C++.
//   xcde.ini fps_overlay_scale    = 50..150 (percent; missing / 0 = 100)
//   xcde.ini fps_overlay_position = top-left | top-right | bottom-left | bottom-right
#pragma once
#include <string.h>

#define PERF_OVERLAY_SCALE_MIN 50
#define PERF_OVERLAY_SCALE_MAX 150
enum { PERF_POS_TOP_LEFT = 0, PERF_POS_TOP_RIGHT = 1, PERF_POS_BOTTOM_LEFT = 2, PERF_POS_BOTTOM_RIGHT = 3, PERF_POS_COUNT = 4 };

static inline int perf_overlay_scale_percent(int v) {
    return v <= 0 ? 100 : v < PERF_OVERLAY_SCALE_MIN ? PERF_OVERLAY_SCALE_MIN : v > PERF_OVERLAY_SCALE_MAX ? PERF_OVERLAY_SCALE_MAX : v;
}
static const char *const perf_overlay_pos_ini[PERF_POS_COUNT] = {"top-left", "top-right", "bottom-left", "bottom-right"};
static inline int perf_overlay_pos_valid(int v) { return v >= 0 && v < PERF_POS_COUNT ? v : PERF_POS_TOP_LEFT; }
static inline int perf_overlay_pos_parse(const char *v) {
    for (int i = 0; i < PERF_POS_COUNT; i++) if (!strcmp(v, perf_overlay_pos_ini[i])) return i;
    return PERF_POS_TOP_LEFT;
}
