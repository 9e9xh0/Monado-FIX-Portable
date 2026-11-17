// [XCDE] GitHub #121: HUD scale. The game lays its HUD out on its 1280x720 frame and every element is its own draw
// (a quad, or one instanced draw per text string) whose vertex constant buffer 3 starts with the element's 2D affine
// transform to clip space (rows a b 0 tx / c d 0 ty, y up). exec.c shrinks each UI draw by s around an anchor picked
// from where the element sits - the screen edge or corner it belongs to, or the centre - by folding that scale into
// the draw's viewport mapping (viewport and scissor follow; the shaders are untouched). Header-only: tests/native.
#pragma once
#include <stdbool.h>
#include <math.h>

// fraction of the frame, per axis: an element whose near side is within it hugs that edge
#define UI_SCALE_EDGE 0.1f
#define UI_SCALE_MIN 50  // (% : smaller and the HUD's text is no longer readable at 1080p)

// xcde.ini ui_scale (percent) -> a valid setting: 100 (the game's own size) when missing or out of range below 1
static inline int ui_scale_percent(int v) { return v <= 0 ? 100 : v < UI_SCALE_MIN ? UI_SCALE_MIN : v > 100 ? 100 : v; }

// the anchor along one axis for an element spanning [lo, hi] of a frame of the given size. An element near an edge
// shrinks towards it (the minimap, the party list, the arts palette, the hints keep hugging their edge); any other
// shrinks in place, around its own centre - markers over people and enemies (the target reticle, name plates) stay
// on them, and the pieces of a component that end up in different groups keep their spacing. Across the width
// (centre_line), an element over the middle of the frame shrinks around the frame's centre: the arts palette and the
// line under it stay centred whatever joined their group this frame (the strips behind them, a corner gauge that
// comes and goes). Down the height there is no such rule: the party list reaches past the middle yet hugs the top.
static inline float ui_scale_anchor_axis_(float lo, float hi, float size, bool centre_line) {
    bool near0 = lo < size * UI_SCALE_EDGE, near1 = hi > size * (1.0f - UI_SCALE_EDGE);
    if (centre_line && (near0 || near1) && lo < size * 0.5f && hi > size * 0.5f) return size * 0.5f;
    if (near0 && near1) return size * 0.5f;
    if (near0) return 0;
    if (near1) return size;
    return 0.5f * (lo + hi);
}
static inline float ui_scale_anchor_x(float lo, float hi, float w) { return ui_scale_anchor_axis_(lo, hi, w, true); }
static inline float ui_scale_anchor_y(float lo, float hi, float h) { return ui_scale_anchor_axis_(lo, hi, h, false); }

// box: the element in guest pixels (x0, y0, x1, y1; y down) on a w x h frame. false: leave the draw alone (it
// covers the frame: a full-screen tint, fade or vignette must keep covering it)
static inline bool ui_scale_anchor(const float box[4], float w, float h, float *ax, float *ay) {
    if (box[2] - box[0] >= 0.9f * w && box[3] - box[1] >= 0.9f * h) return false;
    *ax = ui_scale_anchor_x(box[0], box[2], w);
    *ay = ui_scale_anchor_y(box[1], box[3], h);
    return true;
}

// The element's box from its clip-space transform: m = cb3 floats 0..7 (a b _ tx c d _ ty), the local corners
// lx0..lx1 / ly0..ly1 (or a single point: lx0 == lx1, ly0 == ly1), the draw's guest viewport vp (x, y, w, h).
// false when m does not look like a 2D transform (another shader layout: the draw is not scaled).
static inline bool ui_scale_box(const float m[8], float lx0, float ly0, float lx1, float ly1, const float vp[4], float box[4]) {
    if (m[2] != 0 || m[6] != 0) return false;
    for (int i = 0; i < 8; i++) if (!isfinite(m[i])) return false;
    if (fabsf(m[3]) > 4 || fabsf(m[7]) > 4) return false;
    float cx0 = 1e30f, cy0 = 1e30f, cx1 = -1e30f, cy1 = -1e30f;
    for (int k = 0; k < 4; k++) {
        float lx = (k & 1) ? lx1 : lx0, ly = (k & 2) ? ly1 : ly0;
        float x = m[0] * lx + m[1] * ly + m[3], y = m[4] * lx + m[5] * ly + m[7];
        cx0 = fminf(cx0, x); cx1 = fmaxf(cx1, x); cy0 = fminf(cy0, y); cy1 = fmaxf(cy1, y);
    }
    // clip (y up) -> guest pixels (y down)
    box[0] = vp[0] + (cx0 + 1) * 0.5f * vp[2];
    box[2] = vp[0] + (cx1 + 1) * 0.5f * vp[2];
    box[1] = vp[1] + (1 - cy1) * 0.5f * vp[3];
    box[3] = vp[1] + (1 - cy0) * 0.5f * vp[3];
    return true;
}

// Pieces of one HUD component (the minimap and its icons, a hint list, a dialogue box and its text) must share one
// anchor, or a component on a zone boundary falls apart. The game draws a component's pieces one after another, so
// a draw whose box touches the current group's box (grown by a margin) joins the group and uses its anchor; any
// other draw starts a new group, anchored from its own box. The caller resets the group per frame and target.
typedef struct {
    bool valid;
    float box[4];  // the union of the group's boxes (guest pixels)
    float ax, ay;  // its anchor
} UiScaleGroup;

static inline bool ui_scale_group_anchor(UiScaleGroup *g, const float box[4], float w, float h, float *ax, float *ay) {
    if (box[2] - box[0] >= 0.9f * w && box[3] - box[1] >= 0.9f * h) return false;  // (full frame: not scaled, no group)
    float m = h * 0.045f;  // (32 px of the 1280x720 frame)
    if (g->valid && box[0] <= g->box[2] + m && box[2] >= g->box[0] - m && box[1] <= g->box[3] + m && box[3] >= g->box[1] - m) {
        g->box[0] = fminf(g->box[0], box[0]); g->box[1] = fminf(g->box[1], box[1]);
        g->box[2] = fmaxf(g->box[2], box[2]); g->box[3] = fmaxf(g->box[3], box[3]);
    } else {
        if (!ui_scale_anchor(box, w, h, &g->ax, &g->ay)) return false;
        g->valid = true;
        for (int i = 0; i < 4; i++) g->box[i] = box[i];
    }
    *ax = g->ax; *ay = g->ay;
    return true;
}

// Order-free groups: the boxes of a whole frame are grouped (any two within the margin join, transitively) and each
// group's anchor comes from the group's whole box. The next frame's draws find their box among the last frame's
// (the HUD barely moves between frames) and use that group's anchor; a draw with no match (an element that just
// appeared) falls back to the in-order chaining above.
#define UI_SCALE_MAX_ITEMS 768
typedef struct {
    float box[UI_SCALE_MAX_ITEMS][4];
    int n;
} UiScaleBoxes;
typedef struct {
    UiScaleBoxes cur;                    // this frame's boxes so far
    float prev[UI_SCALE_MAX_ITEMS][4];   // the last frame's boxes
    float pax[UI_SCALE_MAX_ITEMS], pay[UI_SCALE_MAX_ITEMS];  // the anchor of each one's group
    int nprev;
    UiScaleGroup chain;
} UiScaleFrame;

static inline int ui_scale_find_(int *p, int i) { while (p[i] != i) i = p[i] = p[p[i]]; return i; }
static inline bool ui_scale_near_(const float *a, const float *b, float m) {
    return a[0] <= b[2] + m && a[2] >= b[0] - m && a[1] <= b[3] + m && a[3] >= b[1] - m;
}

// a new frame (or target) begins: group the boxes of the one before
static inline void ui_scale_frame_begin(UiScaleFrame *f, float w, float h) {
    static int parent[UI_SCALE_MAX_ITEMS];
    static float gb[UI_SCALE_MAX_ITEMS][4];
    int n = f->cur.n;
    float m = h * 0.065f;  // (47 px of the 1280x720 frame: the party list's rows are 29 and 37 px apart, the hints sit under the area name)
    for (int i = 0; i < n; i++) parent[i] = i;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (ui_scale_near_(f->cur.box[i], f->cur.box[j], m)) {
                int a = ui_scale_find_(parent, i), b = ui_scale_find_(parent, j);
                if (a != b) parent[b] = a;
            }
    for (int i = 0; i < n; i++) { gb[i][0] = gb[i][1] = 1e30f; gb[i][2] = gb[i][3] = -1e30f; }
    for (int i = 0; i < n; i++) {
        int g = ui_scale_find_(parent, i);
        gb[g][0] = fminf(gb[g][0], f->cur.box[i][0]); gb[g][1] = fminf(gb[g][1], f->cur.box[i][1]);
        gb[g][2] = fmaxf(gb[g][2], f->cur.box[i][2]); gb[g][3] = fmaxf(gb[g][3], f->cur.box[i][3]);
    }
    for (int i = 0; i < n; i++) {
        int g = ui_scale_find_(parent, i);
        for (int k = 0; k < 4; k++) f->prev[i][k] = f->cur.box[i][k];
        f->pax[i] = ui_scale_anchor_x(gb[g][0], gb[g][2], w);
        f->pay[i] = ui_scale_anchor_y(gb[g][1], gb[g][3], h);
    }
    f->nprev = n;
    f->cur.n = 0;
    f->chain.valid = false;
}

// this draw's anchor; false: not scaled (it covers the frame)
static inline bool ui_scale_frame_anchor(UiScaleFrame *f, const float box[4], float w, float h, float *ax, float *ay) {
    if (box[2] - box[0] >= 0.9f * w && box[3] - box[1] >= 0.9f * h) return false;
    // the last frame's box this one continues: overlapping (2 px of slack), the nearest centre
    int best = -1;
    float bd = 1e30f, cx = 0.5f * (box[0] + box[2]), cy = 0.5f * (box[1] + box[3]);
    for (int i = 0; i < f->nprev; i++) {
        if (!ui_scale_near_(box, f->prev[i], 2)) continue;
        float dx = 0.5f * (f->prev[i][0] + f->prev[i][2]) - cx, dy = 0.5f * (f->prev[i][1] + f->prev[i][3]) - cy;
        float d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    if (f->cur.n < UI_SCALE_MAX_ITEMS) { for (int k = 0; k < 4; k++) f->cur.box[f->cur.n][k] = box[k]; f->cur.n++; }
    if (best >= 0) { *ax = f->pax[best]; *ay = f->pay[best]; return true; }
    return ui_scale_group_anchor(&f->chain, box, w, h, ax, ay);
}

// Fold "scale by s around (ax, ay) in window pixels" into the draw's guest -> window mapping X = gx * sx + ox.
static inline void ui_scale_fold(float s, float ax, float ay, float *sx, float *sy, float *ox, float *oy) {
    *sx *= s; *sy *= s;
    *ox = s * *ox + (1 - s) * ax;
    *oy = s * *oy + (1 - s) * ay;
}
