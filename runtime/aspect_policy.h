// [XCDE] GitHub #26: window aspect ratios the scene widening handles ("Aspect ratio: Automatic"). Up to 48:9 (three
// 16:9 monitors side by side, e.g. 5760x1080 or 1920x360 for a test); beyond that the game keeps its 16:9 image in the
// centre. A window taller than 4:3 is left alone too, and exactly 16:9 is not a widened shape.
#pragma once
#include <math.h>
#include <stdbool.h>

static inline bool xcde_aspect_widened(double ratio) {
    return ratio >= 4.0 / 3.0 - 0.005 && ratio <= 48.0 / 9.0 + 0.005 &&
           fabs(ratio / (16.0 / 9.0) - 1.0) > 0.005;
}
