// [XCDE] GitHub #121: HUD scale anchors (runtime/ui_scale.h). The boxes are guest pixels on the 1280x720 frame,
// measured from the field and battle HUD (XCDE_UI_SCALE_LOG); no game data.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../../runtime/ui_scale.h"

static const float W = 1280, H = 720;
static UiScaleFrame f;  // (large: not on the stack)

static bool near_(float a, float b) { return fabsf(a - b) < 0.01f; }

// one frame of boxes through the frame grouping; returns the anchors of the last frame's run
static void frame(const float (*b)[4], int n, float *ax, float *ay) {
    ui_scale_frame_begin(&f, W, H);
    for (int i = 0; i < n; i++) {
        bool ok = ui_scale_frame_anchor(&f, b[i], W, H, &ax[i], &ay[i]);
        assert(ok);
    }
}

int main(void) {
    float ax, ay, box[4];

    // settings: missing / 0 = the game's size, clamped to 50..100
    assert(ui_scale_percent(0) == 100 && ui_scale_percent(30) == UI_SCALE_MIN && ui_scale_percent(75) == 75 &&
           ui_scale_percent(150) == 100);

    // per-axis anchors: an edge it hugs, else its own centre; across the width, over the middle = the centre
    assert(ui_scale_anchor_x(1095, 1285, W) == W);                 // minimap: right edge
    assert(ui_scale_anchor_y(-5, 185, H) == 0);                    // ... top edge
    assert(ui_scale_anchor_x(540, 740, W) == 640);                 // reticle alone: in place
    assert(ui_scale_anchor_y(300, 400, H) == 350);
    assert(ui_scale_anchor_x(0, 1084, W) == W / 2);                // the palette grouped with the left gauge: centred
    assert(ui_scale_anchor_y(27, 412, H) == 0);                    // the party list reaches past the middle: still the top
    assert(ui_scale_anchor_x(-1, 1281, W) == W / 2);               // a bar across the screen

    // a full-frame draw (fade, tint) is never scaled
    float full[4] = {0, 0, 1280, 720};
    assert(!ui_scale_anchor(full, W, H, &ax, &ay));

    // the clip-space transform at the start of vertex cbuf 3 (y up) -> guest pixels (y down)
    const float m[8] = {2, 0, 0, 0.859375f, -0.0f, -3.55556f, 0, 0.75f}, vp[4] = {0, 0, 1280, 720};
    assert(ui_scale_box(m, -0.06875f, -0.06875f, 0.06875f, 0.06875f, vp, box));
    assert(fabsf(box[0] - 1101.5f) < 1 && fabsf(box[2] - 1277.5f) < 1 && box[1] < 2 && fabsf(box[3] - 177.5f) < 1);
    const float m3[8] = {1, 0, 0.5f, 0, 0, 1, 0, 0};  // (a z term: some other layout, not a 2D element)
    assert(!ui_scale_box(m3, 0, 0, 1, 1, vp, box));

    // order-free groups: the field HUD's minimap, area name and hints (drawn bottom-up) share the top-right corner
    const float field[][4] = {
        {1095, -5, 1285, 185}, {1027, 196, 1251, 231},                       // minimap, area name
        {1138, 281, 1225, 309}, {1219, 276, 1255, 312},                      // "Auto-run" + icon
        {1095, 257, 1225, 285}, {1219, 252, 1255, 288},                      // "Move Camera" + icon
        {1157, 233, 1225, 261}, {1243, 228, 1279, 264},                      // "Target" + icon
        {1163, 675, 1228, 710}, {1221, 666, 1269, 714},                      // "Story" + icon
    };
    const int nf = (int)(sizeof field / sizeof field[0]);
    float fx[16], fy[16];
    frame(field, nf, fx, fy);  // (first frame: in-order chaining)
    frame(field, nf, fx, fy);  // (second: the first frame's groups)
    for (int i = 0; i < 8; i++) assert(fx[i] == W && fy[i] == 0);
    assert(fx[8] == W && fy[8] == H && fx[9] == W && fy[9] == H);

    // the battle party list: rows 29 and 37 px apart stay one group - one anchor, on the left edge
    const float party[][4] = {{4, 139, 158, 175}, {13, 147, 33, 167}, {16, 204, 26, 216}, {121, 217, 149, 245},
                              {0, 250, 162, 296}, {4, 255, 158, 291}, {118, 333, 146, 361}, {14, 335, 24, 348},
                              {-2, 366, 159, 412}, {1, 371, 156, 407}};
    float px[10], py[10];
    frame(party, 10, px, py);
    frame(party, 10, px, py);
    for (int i = 0; i < 10; i++) assert(px[i] == 0 && py[i] == py[0]);

    // the arts palette with the strip behind the help line and the left gauge: centred, at the bottom
    const float bottom[][4] = {{-1, 640, 639, 720}, {0, 610, 196, 720}, {584, 546, 696, 658}, {523, 680, 946, 710}};
    float bx[4], by[4];
    frame(bottom, 4, bx, by);
    frame(bottom, 4, bx, by);
    for (int i = 0; i < 4; i++) assert(bx[i] == W / 2 && by[i] == H);

    // an element that just appeared (no box last frame) chains in draw order
    frame(bottom, 0, bx, by);
    float lone[4] = {600, 300, 680, 340};
    assert(ui_scale_frame_anchor(&f, lone, W, H, &ax, &ay) && near_(ax, 640) && near_(ay, 320));

    // the fold: X = gx * sx + ox, scaled by s around (ax, ay); the anchor stays put
    float sx = 1.5f, sy = 1.5f, ox = 0, oy = 0;
    ui_scale_fold(0.5f, 1920, 0, &sx, &sy, &ox, &oy);
    assert(near_(1280 * sx + ox, 1920) && near_(0 * sy + oy, 0) && near_(640 * sx + ox, 1440));

    puts("test_ui_scale: ok");
    return 0;
}
