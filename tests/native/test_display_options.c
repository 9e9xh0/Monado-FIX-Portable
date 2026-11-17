// [XCDE] T256/T257/T258: route, SDK queue and native-FPS behavior without GPU/game assets.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../../runtime/aspect_policy.h"
#include "../../runtime/vulkan_policy.h"
#include "../../runtime/fps_meter.h"
#include "../../runtime/dlss_policy.h"
int main(void) {
    bool fallback;
    assert(xcde_vulkan_route(XCDE_VK12 | XCDE_VK13 | XCDE_VK14, 14, 0, &fallback) == 14 && !fallback);
    assert(xcde_vulkan_route(XCDE_VK12 | XCDE_VK13, 14, 12, &fallback) == 12 && !fallback);
    assert(xcde_vulkan_route(XCDE_VK13, 14, 12, &fallback) == 13 && fallback);
    assert(xcde_vulkan_route(XCDE_VK12 | XCDE_VK13 | XCDE_VK14, 13, 14, &fallback) == 13 && fallback);
    assert(!xcde_vulkan_route(0, 14, 0, &fallback));
    uint32_t graphics, compute;
    xcde_dlss_queue_indices(0, 0, &graphics, &compute); assert(graphics == 0 && compute == 0);
    xcde_dlss_queue_indices(2, 3, &graphics, &compute); assert(graphics == 1 && compute == 3);
    xcde_dlss_queue_indices(0, 1, &graphics, &compute); assert(graphics == 0 && compute == 1);
    const int rates[] = {30, 60, 120, 144, 165};
    for (unsigned j = 0; j < sizeof rates / sizeof rates[0]; ++j) {
        XcdeFpsMeter meter = {0};
        for (int i = 0; i <= rates[j] * 2; ++i) xcde_fps_tick(&meter, (uint64_t)i * 1000000000ull / (unsigned)rates[j]);
        assert(fabs(meter.fps - rates[j]) < 0.001);
        assert(fabs(meter.ms - 1000.0 / rates[j]) < 0.001);
        double old = meter.fps; xcde_fps_tick(&meter, meter.last); assert(meter.fps == old);
        xcde_fps_tick(&meter, meter.last + 3000000000ull); assert(!meter.fps && !meter.intervals);
    }
    // [XCDE] GitHub #26: 48:9 triple-monitor windows widen; 16:9, taller-than-4:3 and beyond 48:9 keep the 16:9 image.
    assert(!xcde_aspect_widened(16.0 / 9.0));
    assert(xcde_aspect_widened(4.0 / 3.0));
    assert(xcde_aspect_widened(21.0 / 9.0));
    assert(xcde_aspect_widened(32.0 / 9.0));
    assert(xcde_aspect_widened(5760.0 / 1080.0));
    assert(xcde_aspect_widened(1920.0 / 360.0));
    assert(!xcde_aspect_widened(64.0 / 9.0));
    assert(!xcde_aspect_widened(1.2));
    puts("display options: Vulkan routes, zero-extra SL queues, native FPS and 48:9 aspect passed"); return 0;
}
