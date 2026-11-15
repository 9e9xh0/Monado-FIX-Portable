"""XCDE 1.1.2 instruction patches (contract: docs/contracts/patch-format.md).

The 1.1.2 build keeps the same features as 2.0.0 at different addresses.
The frame limiter is `ml::DevGraphNx::renderSyncSys` (main text 0x693c90):
it spins on the system tick until `interval * display period * 0.997` passed;
the two hooks are the loop head and the function's epilogue (T157).
The graphics configuration is `ml::DrPixlPostParm` (the 0x1220-byte render
config), with the same construction/copy/update boundaries as 2.0.0. Grass
distance scales the MPF visibility radius in `DrMdoCtlMpfAuto::autoMpfCalc`,
and the MPF material far fade in `ml::DrMdoSetup`'s uniform writers. Tree /
rock mesh-LOD transitions are adjusted in the decoded instanced-map prop
buffer (`ml::DrMdoCtlSeamInstMap::initInstMapParts`).
Menu hold/repeat timers take the frame's elapsed seconds
(`game::UIInputSendManager::updateEvents`), walk-transition frame thresholds
are scaled by the frame rate (`game::StateUtil::canTransitionWalk*`), the
scene aspect is widened in `ml::DrCalcViewWork::begin`, and shadow maps size
both pool dimensions and the three cascade defaults in
`ml::DrPixlShadowCacheMan`'s constructor.
Cutscenes are marked by `game::EventCamera` / `game::EvtPlayCamera` and the
direct mouse camera by `game::PlayerCamera` (yaw +0x1f0, pitch +0x1f8).
Patches whose 1.1.2 locations are not verified stay out rather than risk
writing to unrelated code.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "lifter"))
from patchfmt import Patch  # noqa: E402


def _hook_then_original(fn):
    """Call a runtime hook, then the original instruction (which must lift to plain C)."""
    def emit(gen, addr):
        ins = gen.lift(addr)
        if ins.kind != 'op':
            raise ValueError(f'{addr:#x}: hook patch needs a plain instruction, got {ins.kind}')
        return f'{{ extern void {fn}(void); {fn}(); }} {ins.code}'
    return emit


PATCHES = {
    # ml::DevGraphNx::renderSyncSys loop head / exit (same instructions as 2.0.0's limiter).
    0x7000693d18: Patch(0x320003e0, _hook_then_original('xcde_limiter_spin'), 'T157',
                        'mov w0, #1 - limiter loop head'),
    0x7000693e18: Patch(0x6d4123e9, _hook_then_original('xcde_limiter_done'), 'T157',
                        'ldp d9, d8, [sp, #0x10] - limiter exit'),
}


# [XCDE] Graphics options (runtime/hle/graphics.c, graphics_options.h).
# ml::DrPixlPostParm is the game's 0x1220-byte render configuration. The same five boundaries as 2.0.0:
# construction/reset, destruction, a fresh native copy, and the per-view update that applies the overlay.
def _graphics_copy(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError('graphics copy requires a plain epilogue instruction')
    return ('{ extern void xcde_graphics_copy(uint64_t destination); '
            'xcde_graphics_copy(c->x[19]); } ' + ins.code)


def _graphics_finish(gen, addr):
    return _graphics_copy(gen, addr).replace('xcde_graphics_copy', 'xcde_graphics_finish')


def _graphics_begin(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError('graphics begin requires a plain prologue')
    return ('{ extern void xcde_graphics_begin(uint64_t destination); '
            'xcde_graphics_begin(c->x[0]); } ' + ins.code)


def _graphics_construct(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError('graphics constructor requires a plain prologue')
    return ('{ extern void xcde_graphics_forget(uint64_t destination); '
            'xcde_graphics_forget(c->x[0]); } ' + ins.code)


def _graphics_destroy(gen, addr):
    return ('{ extern void xcde_graphics_forget(uint64_t destination); '
            'xcde_graphics_forget(c->x[0]); } return;')


PATCHES[0x70007ff010] = Patch(0xf81e0ff3, _graphics_construct, 'T-GRAPHICS-SCALABILITY',
                              'DrPixlPostParm::reset prologue - forget prior overlay')
PATCHES[0x70007ff9e0] = Patch(0xd65f03c0, _graphics_destroy, 'T-GRAPHICS-SCALABILITY',
                              'DrPixlPostParm destructor - forget prior overlay')
PATCHES[0x70007ffa08] = Patch(0xa9417bfd, _graphics_copy, 'T-GRAPHICS-SCALABILITY',
                              'DrPixlPostParm::operator= epilogue - fresh native copy')
PATCHES[0x70007ffa20] = Patch(0xf81d0ff5, _graphics_begin, 'T-GRAPHICS-SCALABILITY',
                              'DrPixlPostParm::updateSpec prologue - restore prior overlay')
PATCHES[0x70007ffad0] = Patch(0xa9427bfd, _graphics_finish, 'T-GRAPHICS-SCALABILITY',
                              'DrPixlPostParm::updateSpec epilogue - apply per-view overlay')


# [XCDE] Grass draw distance: scale the MPF visibility radius the same way as 2.0.0.
def _foliage_radius(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError('foliage radius patch needs a plain instruction')
    return (ins.code + ' { extern float xcde_foliage_distance(float, float); '
            'float radius_; uint32_t bits_; memcpy(&radius_, &c->v[8], 4); '
            'radius_ = xcde_foliage_distance(0.f, radius_); '
            'memcpy(&bits_, &radius_, 4); c->v[8] = bits_; }')


for _site in (0x7000791490, 0x700079159c):
    PATCHES[_site] = Patch(0xbd400908, _foliage_radius, 'T-FOLIAGE-DISTANCE',
                           'ldr s8, [x8, #8] - scale MPF visibility radius')


# T-FOLIAGE-DISTANCE (shading): the MPF material far fade is rebuilt in ml::DrMdoSetup's uniform writers. s0 is the
# near distance (already defaulted) and s1 the far one; scale the far distance before it is packed, so the material
# fade follows the chosen grass range exactly as 2.0.0 does.
def _foliage_fade(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError('foliage fade patch needs a plain instruction')
    return ('{ extern float xcde_foliage_shading_distance(float, float); '
            'float near_, far_; uint32_t bits_; '
            'memcpy(&near_, &c->v[0], 4); memcpy(&far_, &c->v[1], 4); '
            'far_ = xcde_foliage_shading_distance(near_, far_); '
            'memcpy(&bits_, &far_, 4); c->v[1] = bits_; } ' + ins.code)


for _site in (0x7000790ff0, 0x70007c9d4c):
    PATCHES[_site] = Patch(0xbd0023e0, _foliage_fade, 'T-FOLIAGE-DISTANCE',
                           'str s0, [sp, #0x20] - MPF material far fade')


# T210: frame-count thresholds at the game's own 30 fps. The field follower restarts its walk transition once the
# movement/behavior component (+0x200) counted more than 9 frames; at 60 fps that happens every ~11 frames.
# 1.1.2: game::StateUtil::canTransitionWalkStart / canTransitionWalkEnd (the same two `cmp w8, #9` checks).
def _frames_gt(n):
    def emit(gen, addr):
        return ('{ extern uint32_t xcde_frames_gt(uint32_t n); '
                f'(void)(subs32(c, (uint32_t)c->x[8], xcde_frames_gt({n}))); }}')
    return emit


for _site in (0x70004cce2c, 0x70004cd07c):
    PATCHES[_site] = Patch(0x7100251f, _frames_gt(9), 'T210',
                           'cmp w8, #9 - walk transition frame counter')


# T-MENU-INPUT: game::UIInputSendManager::updateEvents advanced every pad hold/repeat timer by the fixed 1/30 s
# step at UpdateInfo+4 once per presented frame, so at 120 fps taps repeated and held directions scrolled 4x too
# fast. UpdateInfo+8 holds the frame's elapsed seconds. The same 27 timer loads as 2.0.0.
def _menu_input_elapsed(gen, addr):
    return '{ uint64_t ea = c->x[20] + 8; c->v[1] = LD32(ea); }'


_MENU_HOLD_TIME_LOADS = (
    0x700061fb5c, 0x700061fb8c, 0x700061fbb8, 0x700061fbe4,
    0x700061fc10, 0x700061fc3c, 0x700061fc68, 0x700061fc94,
    0x700061fcc0, 0x700061fcec, 0x700061fd18, 0x700061fd44,
    0x700061fd70, 0x700061fd9c, 0x700061fdc8, 0x700061fdf4,
    0x700061fe20, 0x700061fe4c, 0x700061fe78, 0x700061fea4,
    0x700061fed0, 0x700061fefc, 0x700061ff28, 0x700061ff54,
    0x700061ff80, 0x700061ffa8, 0x7000620054,
)
for _site in _MENU_HOLD_TIME_LOADS:
    PATCHES[_site] = Patch(0xbd400681, _menu_input_elapsed, 'T-MENU-INPUT',
                           'ldr s1, [x20, #4] -> elapsed seconds at +8 for menu hold timers')


# T265: the main perspective camera's ml::DrCalcViewWork::begin stores the 16:9 projection aspect; widen it for the
# output when the camera is the field camera. x20 is the view index there in 1.1.2 too.
def _scene_aspect(gen, addr):
    return ('{ extern uint32_t xcde_scene_aspect(uint32_t, uint32_t); '
            'ST32(c->x[19] + 0x4c0, xcde_scene_aspect((uint32_t)c->v[4], (uint32_t)c->x[20])); }')


PATCHES[0x700073abd0] = Patch(0xbd04c264, _scene_aspect, 'T265',
                              'str s4, [x19, #0x4c0] - store scene projection aspect')


# T224: an in-engine cutscene pushes game::EventCamera; its per-frame update marks the cutscene for the limiter.
PATCHES[0x7000078940] = Patch(0xf81d0ff5, _hook_then_original('xcde_note_event_camera'), 'T224',
                              'EventCamera::update - in-engine cutscene active')
PATCHES[0x70000866b0] = Patch(0xf81e0ff3, _hook_then_original('xcde_note_event_camera'), 'T224',
                               'EvtPlayCamera::update - cutscene camera active')

# T236: game::PlayerCamera::update runs only while the player camera is the active view (field / battle, not a
# cutscene); the direct mouse camera writes its yaw / pitch only then (runtime/hle/party.c).
PATCHES[0x700007dfa0] = Patch(0x6db833ed, _hook_then_original('xcde_note_player_camera'), 'T236',
                              'PlayerCamera::update - player camera active')


# T-SHADOW-MAPS: size both native pool-layout calculations and the three cascade defaults. The 1.1.2 defaults are
# loaded in ml::DrPixlShadowCacheMan's constructor as (near float, 1024) pairs; only the high lane (the size) is
# replaced, so the authored cascade distances stay. The independent 256px target remains authored.
def _shadow_pool_dimension(reg):
    def emit(gen, addr):
        return ('{ extern uint32_t xcde_shadow_map_size(void); '
                f'c->x[{reg}] = xcde_shadow_map_size(); }}')
    return emit


for _site, _word, _reg in ((0x70007ebdc4, 0x321603e2, 2), (0x70007ebdc8, 0x321603e3, 3)):
    PATCHES[_site] = Patch(_word, _shadow_pool_dimension(_reg), 'T-SHADOW-MAPS',
                           'native cascade pool dimensions')


def _shadow_default(reg):
    def emit(gen, addr):
        ins = gen.lift(addr)
        if ins.kind != 'op':
            raise ValueError('shadow defaults require a plain load')
        return (ins.code + ' { extern uint32_t xcde_shadow_map_size(void); '
                f'c->v[{reg}]=(c->v[{reg}] & ~((__uint128_t)0xffffffffu<<32)) | ((__uint128_t)xcde_shadow_map_size()<<32); }}')
    return emit


for _site, _word, _reg in ((0x7000800c3c, 0xfd457100, 0), (0x7000800c5c, 0xfd457501, 1),
                           (0x7000800c7c, 0xfd44a901, 1)):
    PATCHES[_site] = Patch(_word, _shadow_default(_reg), 'T-SHADOW-MAPS',
                           'native cascade resolution default; preserve near/far floats')


# T-PROP-LOD: decoded prop buffer and its size from the selected 64-byte map stream record are available before the
# stream's native setup consumes it. 1.1.2: ml::DrMdoCtlSeamInstMap::initInstMapParts, x22 the decoded buffer and x8 the
# record (the same 64-byte stride, lsl #6, as 2.0.0).
def _prop_lod_prepare(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError('prop LOD hook requires a plain load')
    return ('{ extern void xcde_prop_lod(uint64_t data,uint32_t size); '
            'xcde_prop_lod(c->x[22], LD32(c->x[8] + 0x38)); } ' + ins.code)


PATCHES[0x70007ba62c] = Patch(0xb9403908, _prop_lod_prepare, 'T-PROP-LOD',
                              'ldr w8, [x8, #0x38] - instanced map prop buffer and size')
