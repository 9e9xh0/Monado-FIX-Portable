// [XCDE] T260: pure timing/cache policies, testable without the game or a Vulkan driver.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
static inline bool xcde_repeat_has_budget(uint64_t start, uint64_t now, int fps, double hz) {
    if (!start || now < start || fps <= 0 || hz <= 0) return false;
    double budget = 1e9 / fps, refresh = 1e9 / hz;
    return (double)(now-start) + refresh <= budget + 250000.0;
}
// [XCDE] The game limiter's target for the FPS setting (framepace.c). Re-read whenever the setting changes, so the F1
// menu applies a new target live: interval -1 keeps the game's own 30 Hz interval (restored after a forced target),
// and period_scale 0 leaves the limiter's original period constant unchanged.
typedef struct { int interval; float period_scale; } XcdeFpsTarget;
static inline XcdeFpsTarget xcde_fps_target(int fps) {
    XcdeFpsTarget t = {1, fps == 0 ? 0.02f : fps > 60 ? 60.0f / (float)fps : 0.0f};
    if (fps == 30) { t.interval = -1; t.period_scale = 0.0f; }
    return t;
}
static inline uint64_t xcde_timestamp_delta(uint64_t start, uint64_t end, unsigned bits) {
    if (!bits) return 0;
    return bits >= 64 ? end-start : (end-start) & ((1ull << bits)-1);
}
static inline int xcde_fd_slot(const int *files, const int *users, const uint64_t *ages, int ways, int wanted) {
    int victim=-1;
    for (int i=0;i<ways;i++) {
        if (files[i]==wanted) return i;
        if (!users[i] && (victim<0 || ages[i]<ages[victim])) victim=i;
    }
    return victim;
}
