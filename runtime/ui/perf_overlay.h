// [XCDE] GitHub #39: the on-screen performance overlay (xcde.ini fps_overlay = 1, fps_overlay_style = detailed | simple).
// Included once, by menu_vk.cpp (the F1 menu's ImGui context draws it), in the F1 menu's look: navy panel, teal section
// headings and values, crimson for trouble. Detailed puts the frame first (FPS, 1% / 0.1% lows, frame-time graph, GPU
// time, CPU waits), then hardware load (GPU / VRAM / CPU / RAM) and the render path (resolutions, upscaler, limit,
// presentation).
//
// Threads: perf_frame_tick and perf_overlay_draw run on the render thread. CPU / RAM / GPU telemetry is read by a
// small background thread (2 Hz, only while the detailed overlay is shown): NVML can take milliseconds per call, which
// would show up as spikes in the very graph it sits next to. GPU temperature / power / clock / load come from NVML
// (NVIDIA, Windows and Linux) or amdgpu's sysfs (AMD, Linux); elsewhere that row shows what Vulkan knows (VRAM).
#pragma once
#include <algorithm>
#include <atomic>
#include <math.h>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include "imgui.h"
#include "theme.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#include <psapi.h>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif
#else
#include <dirent.h>
#include <unistd.h>
#endif
#if !defined(_MSC_VER) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif

// ------------------------------------------------------------------------------------------------ frame history
// Native frame intervals (ms), one per game frame (menu_fps_tick): never repeated presents or generated frames.
enum { PERF_HIST = 4096 };
static float perf_hist[PERF_HIST];
static uint32_t perf_hist_head, perf_hist_n;
static uint64_t perf_last_tick_ns, perf_ticks;
static int perf_game_w, perf_game_h;

static void perf_frame_tick(uint64_t now) {
    if (perf_last_tick_ns && now > perf_last_tick_ns) {
        uint64_t d = now - perf_last_tick_ns;
        if (d < 2000000000ull) {  // (longer: a loading pause / breakpoint, not a frame worth graphing)
            perf_hist[perf_hist_head] = (float)(d / 1e6);
            perf_hist_head = (perf_hist_head + 1) % PERF_HIST;
            if (perf_hist_n < PERF_HIST) perf_hist_n++;
        }
    }
    perf_last_tick_ns = now;
    perf_ticks++;
}
static float perf_hist_at(uint32_t back) { return perf_hist[(perf_hist_head + PERF_HIST - 1 - back) % PERF_HIST]; }

// Values shown, refreshed twice a second (numbers that change every frame cannot be read).
struct PerfView {
    double low1, low01, max_ms;       // 1% / 0.1% low (FPS of the 99th / 99.9th percentile frame time), worst frame (ms)
    double gpu_ms = -1;               // GPU time per native frame (timestamps), -1 unknown
    bool gpu_compiling;               // vk_native_gpu_ms said shader builds are pending (GPU time not representative)
    double gpu_wait_ms, shader_wait_ms;  // CPU time per frame blocked on the GPU / on shader + pipeline builds
    bool have;
};
static PerfView perf_view;

static void perf_update_view(uint64_t now, bool detailed) {
    static uint64_t at, ticks_at, gw_at, sw_at;
    static double gpu_sum; static int gpu_n; static bool gpu_compiling;
    if (detailed) {
        double g = vk_native_gpu_ms();
        if (g >= 0) gpu_sum += g, gpu_n++;
        else if (g == -2) gpu_compiling = true;
    }
    if (at && now - at < 500000000ull) return;
    uint64_t gw = 0, sw = 0;
    vk_overlay_wait_totals(&gw, &sw);
    uint64_t frames = perf_ticks - ticks_at;
    if (at && frames) {
        perf_view.gpu_wait_ms = (double)(gw - gw_at) / 1e6 / (double)frames;
        perf_view.shader_wait_ms = (double)(sw - sw_at) / 1e6 / (double)frames;
    }
    if (gpu_n) perf_view.gpu_ms = gpu_sum / gpu_n;
    perf_view.gpu_compiling = gpu_compiling && !gpu_n;
    gpu_sum = 0; gpu_n = 0; gpu_compiling = false;
    at = now; ticks_at = perf_ticks; gw_at = gw; sw_at = sw;
    // lows and worst frame over the last 10 seconds of frames
    static std::vector<float> w;
    w.clear();
    double sum = 0;
    for (uint32_t i = 0; i < perf_hist_n && sum < 10000.0; i++) { float v = perf_hist_at(i); w.push_back(v); sum += v; }
    perf_view.have = w.size() >= 8;
    if (!perf_view.have) return;
    auto pct = [&](double p) {
        size_t k = std::min(w.size() - 1, (size_t)floor((double)w.size() * p));
        std::nth_element(w.begin(), w.begin() + k, w.end());
        return (double)w[k];
    };
    perf_view.max_ms = *std::max_element(w.begin(), w.end());
    perf_view.low1 = 1000.0 / std::max(0.001, pct(0.99));
    perf_view.low01 = 1000.0 / std::max(0.001, pct(0.999));
}

// ------------------------------------------------------------------------------------------------ system telemetry
struct PerfSys {
    float cpu = -1, cpu_game = -1;                         // % of all cores (the game: its own process)
    double ram_used = -1, ram_total = -1, ram_game = -1;   // bytes
    float gpu_util = -1, gpu_temp = -1, gpu_watts = -1, gpu_mhz = -1;
    double vram_used = -1, vram_total = -1;                // whole GPU (telemetry); the game's own share comes from VMA
};
static std::mutex perf_sys_lock;
static PerfSys perf_sys_shared;
static std::atomic<bool> perf_sys_wanted{false};
static char perf_cpu_name[96];
static char perf_gpu_uuid[33];      // active adapter (lowercase hex, as gpu_probe writes it)
static uint32_t perf_gpu_vendor, perf_gpu_device;

static void perf_trim(char *s) {
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == '\t')) s[--n] = 0;
    for (char *p = s; *p; p++) while (p[0] == ' ' && p[1] == ' ') memmove(p, p + 1, strlen(p));  // ("Intel(R)  Core")
}
static void perf_read_cpu_name(void) {
    perf_cpu_name[0] = 0;
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    unsigned r[12] = {0};
#if defined(_MSC_VER)
    int top[4]; __cpuid(top, 0x80000000);
    if ((unsigned)top[0] >= 0x80000004u) for (int i = 0; i < 3; i++) __cpuid((int *)&r[i * 4], 0x80000002 + i);
#else
    if (__get_cpuid_max(0x80000000u, nullptr) >= 0x80000004u)
        for (unsigned i = 0; i < 3; i++) __get_cpuid(0x80000002u + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
#endif
    memcpy(perf_cpu_name, r, 48); perf_cpu_name[48] = 0;
#endif
#ifdef __linux__
    if (!perf_cpu_name[0]) {  // (ARM: no brand string; /proc/cpuinfo may name it)
        if (FILE *f = fopen("/proc/cpuinfo", "r")) {
            char line[256];
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "model name", 10) || !strncmp(line, "Model", 5)) {
                    if (const char *c = strchr(line, ':')) { snprintf(perf_cpu_name, sizeof perf_cpu_name, "%s", c + 1); break; }
                }
            fclose(f);
        }
    }
#endif
    perf_trim(perf_cpu_name);
}

// ---- NVML (NVIDIA): loaded at run time, nothing to link or ship
typedef int (*PfnNvmlVoid)(void);
typedef int (*PfnNvmlCount)(unsigned *);
typedef int (*PfnNvmlHandle)(unsigned, void **);
typedef int (*PfnNvmlUuid)(void *, char *, unsigned);
typedef int (*PfnNvmlU32)(void *, unsigned *);
typedef int (*PfnNvmlU32Arg)(void *, int, unsigned *);
struct PerfNvmlUtil { unsigned gpu, memory; };
struct PerfNvmlMem { unsigned long long total, free, used; };
typedef int (*PfnNvmlUtil)(void *, PerfNvmlUtil *);
typedef int (*PfnNvmlMem)(void *, PerfNvmlMem *);
static struct {
    void *dev;
    PfnNvmlU32Arg temp, clock;
    PfnNvmlU32 power;
    PfnNvmlUtil util;
    PfnNvmlMem mem;
} perf_nv;
static bool perf_hex_match(const char *nvml_uuid, const char *vk_hex) {  // "GPU-1a2b3c4d-...." vs "1a2b3c4d...."
    char hex[40]; int n = 0;
    const char *p = !strncmp(nvml_uuid, "GPU-", 4) ? nvml_uuid + 4 : nvml_uuid;
    for (; *p && n < 39; p++) if (*p != '-') hex[n++] = (char)((*p >= 'A' && *p <= 'F') ? *p + 32 : *p);
    hex[n] = 0;
    return n == 32 && !strcmp(hex, vk_hex);
}
static bool perf_nvml_open(void) {
#ifdef _WIN32
    SDL_SharedObject *so = SDL_LoadObject("nvml.dll");
#else
    SDL_SharedObject *so = SDL_LoadObject("libnvidia-ml.so.1");
#endif
    if (!so) return false;
    auto init = (PfnNvmlVoid)SDL_LoadFunction(so, "nvmlInit_v2");
    auto count = (PfnNvmlCount)SDL_LoadFunction(so, "nvmlDeviceGetCount_v2");
    auto handle = (PfnNvmlHandle)SDL_LoadFunction(so, "nvmlDeviceGetHandleByIndex_v2");
    auto uuid = (PfnNvmlUuid)SDL_LoadFunction(so, "nvmlDeviceGetUUID");
    perf_nv.temp = (PfnNvmlU32Arg)SDL_LoadFunction(so, "nvmlDeviceGetTemperature");
    perf_nv.clock = (PfnNvmlU32Arg)SDL_LoadFunction(so, "nvmlDeviceGetClockInfo");
    perf_nv.power = (PfnNvmlU32)SDL_LoadFunction(so, "nvmlDeviceGetPowerUsage");
    perf_nv.util = (PfnNvmlUtil)SDL_LoadFunction(so, "nvmlDeviceGetUtilizationRates");
    perf_nv.mem = (PfnNvmlMem)SDL_LoadFunction(so, "nvmlDeviceGetMemoryInfo");
    unsigned n = 0;
    if (!init || !count || !handle || init() != 0 || count(&n) != 0 || !n) return false;
    void *only = nullptr;
    for (unsigned i = 0; i < n; i++) {
        void *d = nullptr; char id[96] = {0};
        if (handle(i, &d) != 0) continue;
        if (n == 1) only = d;
        if (uuid && uuid(d, id, sizeof id) == 0 && perf_hex_match(id, perf_gpu_uuid)) { perf_nv.dev = d; break; }
    }
    if (!perf_nv.dev) perf_nv.dev = only;  // (one NVIDIA GPU: it is the one Vulkan uses)
    return perf_nv.dev != nullptr;
}
static void perf_nvml_sample(PerfSys &s) {
    unsigned v = 0;
    if (perf_nv.temp && perf_nv.temp(perf_nv.dev, 0 /*NVML_TEMPERATURE_GPU*/, &v) == 0) s.gpu_temp = (float)v;
    if (perf_nv.clock && perf_nv.clock(perf_nv.dev, 0 /*NVML_CLOCK_GRAPHICS*/, &v) == 0) s.gpu_mhz = (float)v;
    if (perf_nv.power && perf_nv.power(perf_nv.dev, &v) == 0) s.gpu_watts = v / 1000.0f;
    PerfNvmlUtil u;
    if (perf_nv.util && perf_nv.util(perf_nv.dev, &u) == 0) s.gpu_util = (float)u.gpu;
    PerfNvmlMem m;
    if (perf_nv.mem && perf_nv.mem(perf_nv.dev, &m) == 0) { s.vram_used = (double)m.used; s.vram_total = (double)m.total; }
}

#ifdef __linux__
// ---- amdgpu sysfs (AMD on Linux): /sys/class/drm/cardN/device/{gpu_busy_percent, mem_info_vram_*, hwmon/hwmonM/*}
static char perf_amd_dev[128], perf_amd_hwmon[192];
static bool perf_read_num(const char *path, double *out) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fscanf(f, "%lf", out) == 1;
    fclose(f);
    return ok;
}
static bool perf_read_hex(const char *path, unsigned *out) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fscanf(f, "%x", out) == 1;
    fclose(f);
    return ok;
}
static bool perf_amd_open(void) {
    DIR *d = opendir("/sys/class/drm");
    if (!d) return false;
    while (struct dirent *e = readdir(d)) {
        if (strncmp(e->d_name, "card", 4) || strchr(e->d_name, '-')) continue;  // (cardN, not cardN-DP-1)
        char p[256]; unsigned vendor = 0, device = 0;
        snprintf(p, sizeof p, "/sys/class/drm/%s/device/vendor", e->d_name);
        if (!perf_read_hex(p, &vendor) || vendor != 0x1002) continue;
        snprintf(p, sizeof p, "/sys/class/drm/%s/device/device", e->d_name);
        if (!perf_read_hex(p, &device) || device != perf_gpu_device) continue;
        snprintf(perf_amd_dev, sizeof perf_amd_dev, "/sys/class/drm/%s/device", e->d_name);
        break;
    }
    closedir(d);
    if (!perf_amd_dev[0]) return false;
    char p[192];
    snprintf(p, sizeof p, "%s/hwmon", perf_amd_dev);
    if (DIR *h = opendir(p)) {
        while (struct dirent *e = readdir(h))
            if (!strncmp(e->d_name, "hwmon", 5)) { snprintf(perf_amd_hwmon, sizeof perf_amd_hwmon, "%s/%s", p, e->d_name); break; }
        closedir(h);
    }
    return true;
}
static void perf_amd_sample(PerfSys &s) {
    char p[256]; double v;
    snprintf(p, sizeof p, "%s/gpu_busy_percent", perf_amd_dev);
    if (perf_read_num(p, &v)) s.gpu_util = (float)v;
    snprintf(p, sizeof p, "%s/mem_info_vram_used", perf_amd_dev);
    if (perf_read_num(p, &v)) s.vram_used = v;
    snprintf(p, sizeof p, "%s/mem_info_vram_total", perf_amd_dev);
    if (perf_read_num(p, &v)) s.vram_total = v;
    if (!perf_amd_hwmon[0]) return;
    snprintf(p, sizeof p, "%s/temp1_input", perf_amd_hwmon);
    if (perf_read_num(p, &v)) s.gpu_temp = (float)(v / 1000.0);
    snprintf(p, sizeof p, "%s/power1_average", perf_amd_hwmon);
    if (perf_read_num(p, &v) && v > 0) s.gpu_watts = (float)(v / 1e6);
    else { snprintf(p, sizeof p, "%s/power1_input", perf_amd_hwmon); if (perf_read_num(p, &v)) s.gpu_watts = (float)(v / 1e6); }
    snprintf(p, sizeof p, "%s/freq1_input", perf_amd_hwmon);
    if (perf_read_num(p, &v)) s.gpu_mhz = (float)(v / 1e6);
}
#endif

// ---- CPU load and memory (system and this process)
static void perf_cpu_mem_sample(PerfSys &s) {
    static uint64_t last_total, last_idle, last_proc;
#ifdef _WIN32
    FILETIME idle, kern, user, c, e, pk, pu;
    auto u64 = [](const FILETIME &f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    if (GetSystemTimes(&idle, &kern, &user) && GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu)) {
        uint64_t total = u64(kern) + u64(user), id = u64(idle), proc = u64(pk) + u64(pu);  // (kernel time includes idle)
        if (last_total && total > last_total) {
            double dt = (double)(total - last_total);
            s.cpu = (float)(100.0 * (1.0 - (double)(id - last_idle) / dt));
            s.cpu_game = (float)(100.0 * (double)(proc - last_proc) / dt);
        }
        last_total = total; last_idle = id; last_proc = proc;
    }
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) { s.ram_total = (double)ms.ullTotalPhys; s.ram_used = (double)(ms.ullTotalPhys - ms.ullAvailPhys); }
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) s.ram_game = (double)pmc.WorkingSetSize;
#elif defined(__linux__)
    if (FILE *f = fopen("/proc/stat", "r")) {
        unsigned long long v[8] = {0};
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) {
            uint64_t total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7], id = v[3] + v[4], proc = 0;
            if (FILE *p = fopen("/proc/self/stat", "r")) {  // utime, stime: fields 14, 15 (same clock ticks as /proc/stat)
                char buf[1024]; size_t n = fread(buf, 1, sizeof buf - 1, p); buf[n] = 0; fclose(p);
                if (const char *q = strrchr(buf, ')')) {
                    unsigned long long ut = 0, st = 0;
                    if (sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2) proc = ut + st;
                }
            }
            if (last_total && total > last_total) {
                double dt = (double)(total - last_total);
                s.cpu = (float)(100.0 * (1.0 - (double)(id - last_idle) / dt));
                if (proc && last_proc) s.cpu_game = (float)(100.0 * (double)(proc - last_proc) / dt);
            }
            last_total = total; last_idle = id; last_proc = proc;
        }
        fclose(f);
    }
    if (FILE *f = fopen("/proc/meminfo", "r")) {
        char line[128]; unsigned long long kb, total = 0, avail = 0;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) total = kb;
            else if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) avail = kb;
        }
        fclose(f);
        if (total) { s.ram_total = total * 1024.0; s.ram_used = (double)(total - avail) * 1024.0; }
    }
    if (FILE *f = fopen("/proc/self/statm", "r")) {
        unsigned long long size, rss;
        if (fscanf(f, "%llu %llu", &size, &rss) == 2) s.ram_game = (double)rss * (double)sysconf(_SC_PAGESIZE);
        fclose(f);
    }
#else
    (void)s; (void)last_total; (void)last_idle; (void)last_proc;
#endif
}

static void perf_sys_thread(void) {
    perf_read_cpu_name();
    int src = 0;  // 1 NVML, 2 amdgpu sysfs
    if (perf_gpu_vendor == 0x10de && perf_nvml_open()) src = 1;
#ifdef __linux__
    else if (perf_gpu_vendor == 0x1002 && perf_amd_open()) src = 2;
#endif
    for (;;) {
        if (perf_sys_wanted.load()) {
            PerfSys s;
            perf_cpu_mem_sample(s);
            if (src == 1) perf_nvml_sample(s);
#ifdef __linux__
            if (src == 2) perf_amd_sample(s);
#endif
            std::lock_guard<std::mutex> g(perf_sys_lock);
            perf_sys_shared = s;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
static PerfSys perf_sys_get(void) {
    static bool started;
    if (!started) {
        started = true;
        perf_gpu_vendor = g_vk.props.vendorID; perf_gpu_device = g_vk.props.deviceID;
        for (uint32_t i = 0; i < g_vk_adapter_count; i++)
            if (g_vk_adapters[i].info.index == g_vk.phys_index) snprintf(perf_gpu_uuid, sizeof perf_gpu_uuid, "%s", g_vk_adapters[i].info.uuid);
        std::thread(perf_sys_thread).detach();
    }
    std::lock_guard<std::mutex> g(perf_sys_lock);
    return perf_sys_shared;
}

// ------------------------------------------------------------------------------------------------ drawing
static ImFont *perf_font_big;  // the UI font at ~2x (menu_vk.cpp loads it next to the normal one)
static const char *perf_l(const char *en, const char *es) { return g_settings.ui_lang == 1 ? en : es; }
static ImU32 perf_col(unsigned rgb, float a = 1) { ImVec4 c = monado_rgb(rgb); c.w = a; return ImGui::GetColorU32(c); }
enum : unsigned { PERF_TEAL = 0x5FD4DE, PERF_GOOD = 0x6EE0A0, PERF_WARN = 0xF2B84B, PERF_BAD = 0xF0506A, PERF_TRACK = 0x1B2833 };

static int perf_target_fps(void) {
    int t = xcde_present_fps();
    if (t <= 0) { float hz = vk_display_hz_cached(); t = hz > 0 ? (int)lroundf(hz) : 0; }
    return t;
}
static unsigned perf_fps_rgb(double fps, int target) {
    if (target <= 0 || fps <= 0) return PERF_TEAL;
    double r = fps / target;
    return r >= 0.95 ? PERF_GOOD : r >= 0.75 ? PERF_WARN : PERF_BAD;
}
static unsigned perf_load_rgb(float frac) { return frac < 0.80f ? PERF_TEAL : frac < 0.95f ? PERF_WARN : PERF_BAD; }

// A small rounded chip: dim label then value ("1% low  98").
static void perf_chip(const char *label, const char *value, unsigned value_rgb, float s) {
    ImVec2 pad(7 * s, 2 * s);
    ImVec2 ls = ImGui::CalcTextSize(label), vs = ImGui::CalcTextSize(value);
    float gap = 5 * s;
    ImVec2 p = ImGui::GetCursorScreenPos(), size(ls.x + gap + vs.x + pad.x * 2, ls.y + pad.y * 2);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), perf_col(PERF_TRACK, 0.9f), size.y * 0.5f);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    dl->AddText(ImVec2(p.x + pad.x + ls.x + gap, p.y + pad.y), perf_col(value_rgb), value);
    ImGui::Dummy(size);
}

// A label / value / note row with an optional thin load bar under the value (frac < 0: none).
static void perf_row(const char *label, const char *value, const char *note, float frac, unsigned rgb, float s) {
    float x0 = ImGui::GetCursorPosX(), em = ImGui::GetFontSize();
    float col_value = x0 + em * 8.2f, col_note = x0 + em * 14.6f;
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(col_value);
    ImVec2 vp = ImGui::GetCursorScreenPos();
    ImGui::TextColored(monado_rgb(rgb == PERF_TEAL ? 0xE8EEF2 : rgb), "%s", value);
    if (note && *note) { ImGui::SameLine(col_note); ImGui::TextDisabled("%s", note); }
    if (frac >= 0) {
        float w = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - vp.x;
        float y = vp.y + em + 2 * s, h = 2.5f * s;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(vp.x, y), ImVec2(vp.x + w, y + h), perf_col(PERF_TRACK), h);
        float f = frac < 0 ? 0 : frac > 1 ? 1 : frac;
        unsigned bar = perf_load_rgb(frac) != PERF_TEAL ? perf_load_rgb(frac) : rgb;  // (near full: amber / red)
        if (f > 0) dl->AddRectFilled(ImVec2(vp.x, y), ImVec2(vp.x + w * f, y + h), perf_col(bar), h);
        ImGui::Dummy(ImVec2(0, 3 * s));
    }
}

static void perf_section(const char *title) {
    ImGui::PushStyleColor(ImGuiCol_Text, monado_teal());
    ImGui::PushStyleColor(ImGuiCol_Separator, monado_rgb(0x21414A));
    ImGui::SeparatorText(title);
    ImGui::PopStyleColor(2);
}

// Frame-time graph: the last frames as a line over a soft fill, the frame budget as a dashed line, spikes in red.
static void perf_graph(float width, float height, int target, float s) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + width, p.y + height);
    dl->AddRectFilled(p, q, perf_col(0x0B121A, 0.85f), 8 * s);
    int n = (int)std::min<uint32_t>(perf_hist_n, 360);
    double budget = target > 0 ? 1000.0 / target : 0, avg = 0;
    float peak = 0;
    for (int i = 0; i < n; i++) { float v = perf_hist_at((uint32_t)i); peak = std::max(peak, v); avg += v; }
    avg = n ? avg / n : 0;
    static float scale = 0;  // (eases toward the target so the graph doesn't jump with every spike)
    float want = (float)std::max({budget * 2.0, avg * 2.0, (double)peak * 1.1, 8.0});
    scale = scale ? scale + (want - scale) * 0.08f : want;
    if (want > scale) scale = want;
    float in = 6 * s, gx = p.x + in, gw = width - in * 2, gy = p.y + in, gh = height - in * 2;
    auto ypos = [&](double ms) { return gy + gh - (float)std::min(1.0, ms / scale) * gh; };
    if (budget > 0) {
        float y = ypos(budget);
        for (float x = gx; x < gx + gw; x += 9 * s) dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + 5 * s, gx + gw), y), perf_col(PERF_TEAL, 0.35f), 1.0f * s);
    }
    if (n >= 2) {
        float step = gw / (float)(std::max(n, 120) - 1);
        ImU32 fill = perf_col(PERF_TEAL, 0.14f), line = perf_col(PERF_TEAL), spike = perf_col(PERF_BAD);
        float base = gy + gh;
        for (int i = 0; i + 1 < n; i++) {  // (oldest left, newest right)
            float a = perf_hist_at((uint32_t)(n - 1 - i)), b = perf_hist_at((uint32_t)(n - 2 - i));
            float x0 = gx + gw - (n - 1 - i) * step, x1 = x0 + step;
            float y0 = ypos(a), y1 = ypos(b);
            dl->AddQuadFilled(ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(x1, base), ImVec2(x0, base), fill);
            bool hot = b > avg * 1.6 && b > (budget > 0 ? budget * 1.25 : 0);
            dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), hot ? spike : line, 1.4f * s);
        }
    }
    char t[32];
    if (budget > 0) {  // legend: the dashed line is the frame budget (the top of the scale is left unlabelled)
        snprintf(t, sizeof t, "%.1f ms %s", budget, perf_l("budget", "objetivo"));
        float lx = gx + 2 * s, ly = gy + 1 * s, th = ImGui::GetFontSize();
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddRectFilled(ImVec2(lx - 2 * s, ly), ImVec2(lx + 22 * s + ts.x + 4 * s, ly + th), perf_col(0x0B121A, 0.85f), 4 * s);
        for (float x = lx; x < lx + 16 * s; x += 9 * s) dl->AddLine(ImVec2(x, ly + th * 0.5f), ImVec2(x + 5 * s, ly + th * 0.5f), perf_col(PERF_TEAL, 0.6f), 1.0f * s);
        dl->AddText(ImVec2(lx + 22 * s, ly), ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
    }
    ImGui::Dummy(ImVec2(width, height));
}

static void perf_fmt_gb(char *o, size_t n, double bytes) { snprintf(o, n, "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0)); }

static void perf_draw_simple(float s, int target) {
    ImGui::SetNextWindowPos(ImVec2(12 * s, 12 * s), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * s, 6 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, (ImGui::GetFontSize() + 12 * s) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.37f, 0.83f, 0.87f, 0.30f));
    ImGui::Begin("##perf-simple", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    if (fps_meter.fps > 0) {
        ImGui::TextColored(monado_rgb(perf_fps_rgb(fps_meter.fps, target)), "%.0f", fps_meter.fps);
        ImGui::SameLine(0, 4 * s); ImGui::TextDisabled("FPS");
        ImGui::SameLine(0, 10 * s); ImGui::Text("%.1f", fps_meter.ms);
        ImGui::SameLine(0, 4 * s); ImGui::TextDisabled("ms");
    } else ImGui::TextDisabled("FPS --");
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

static void perf_draw_detailed(float s, int target, uint32_t win_w, uint32_t win_h) {
    PerfSys sys = perf_sys_get();
    const float em = ImGui::GetFontSize(), width = em * 21.0f;
    ImGui::SetNextWindowPos(ImVec2(12 * s, 12 * s), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.84f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s, 12 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * s, 3 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextPadding, ImVec2(0, 3 * s));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.37f, 0.83f, 0.87f, 0.30f));
    ImGui::Begin("##perf-detailed", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    char a[64], b[64];

    // ---- headline: FPS (big), frame time on the right
    {
        ImVec2 top = ImGui::GetCursorScreenPos();
        // crimson accent stripe on the left edge, as the launcher marks the selected page
        ImVec2 wp = ImGui::GetWindowPos();
        dl->AddRectFilled(ImVec2(wp.x, top.y + 2 * s), ImVec2(wp.x + 3 * s, top.y + em * 2.0f), perf_col(0xBA213B), 2 * s);
        if (perf_font_big) ImGui::PushFont(perf_font_big);
        if (fps_meter.fps > 0) ImGui::TextColored(monado_rgb(perf_fps_rgb(fps_meter.fps, target)), "%.0f", fps_meter.fps);
        else ImGui::TextDisabled("--");
        if (perf_font_big) ImGui::PopFont();
        ImVec2 big = ImGui::GetItemRectSize();
        // the small labels sit on the big number's baseline
        float baseline = top.y + (perf_font_big ? perf_font_big->Ascent : ImGui::GetFont()->Ascent);
        float small_top = baseline - ImGui::GetFont()->Ascent;
        ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        float x = top.x + big.x + 6 * s;
        dl->AddText(ImVec2(x, small_top), dim, "FPS");
        if (target > 0) {
            snprintf(b, sizeof b, perf_l("target %d", "objetivo %d"), target);
            dl->AddText(ImVec2(x, small_top - em * 1.05f), dim, b);
        }
        snprintf(a, sizeof a, "%.2f ms", fps_meter.ms);
        float right = wp.x + width - ImGui::GetStyle().WindowPadding.x;
        dl->AddText(ImVec2(right - ImGui::CalcTextSize(a).x, small_top), ImGui::GetColorU32(ImGuiCol_Text), a);
        const char *ft = perf_l("frame time", "tiempo por frame");
        dl->AddText(ImVec2(right - ImGui::CalcTextSize(ft).x, small_top - em * 1.05f), dim, ft);
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + big.y + 4 * s));
    }
    if (perf_view.have) {
        snprintf(a, sizeof a, "%.0f", perf_view.low1); perf_chip(perf_l("1% low", "1% mín"), a, perf_fps_rgb(perf_view.low1, target), s);
        ImGui::SameLine(0, 5 * s);
        snprintf(a, sizeof a, "%.0f", perf_view.low01); perf_chip(perf_l("0.1% low", "0.1% mín"), a, perf_fps_rgb(perf_view.low01, target), s);
        ImGui::SameLine(0, 5 * s);
        snprintf(a, sizeof a, "%.1f ms", perf_view.max_ms);
        perf_chip(perf_l("max", "máx"), a, target > 0 && perf_view.max_ms > 2000.0 / target ? PERF_BAD : 0xE8EEF2, s);
    }
    ImGui::Dummy(ImVec2(0, 2 * s));
    perf_graph(width - ImGui::GetStyle().WindowPadding.x * 2, em * 3.4f, target, s);

    // ---- where the frame time goes
    perf_section(perf_l("FRAME", "FRAME"));
    double frame = fps_meter.ms > 0 ? fps_meter.ms : 0;
    if (perf_view.gpu_ms >= 0) {
        snprintf(a, sizeof a, "%.2f ms", perf_view.gpu_ms);
        snprintf(b, sizeof b, "%s", perf_view.gpu_compiling ? perf_l("compiling", "compilando") : "");
        perf_row(perf_l("GPU busy", "GPU ocupada"), a, b, frame > 0 ? (float)(perf_view.gpu_ms / frame) : -1, PERF_TEAL, s);
    } else perf_row(perf_l("GPU busy", "GPU ocupada"), "--", perf_view.gpu_compiling ? perf_l("compiling", "compilando") : "", -1, PERF_TEAL, s);
    snprintf(a, sizeof a, "%.2f ms", perf_view.gpu_wait_ms);
    perf_row(perf_l("Waiting for GPU", "Esperando a la GPU"), a, "", frame > 0 ? (float)(perf_view.gpu_wait_ms / frame) : -1,
             perf_view.gpu_wait_ms > 1.0 ? PERF_WARN : PERF_TEAL, s);
    snprintf(a, sizeof a, "%.2f ms", perf_view.shader_wait_ms);
    perf_row(perf_l("Waiting for shaders", "Esperando shaders"), a, "", frame > 0 ? (float)(perf_view.shader_wait_ms / frame) : -1,
             perf_view.shader_wait_ms > 0.5 ? PERF_BAD : PERF_TEAL, s);

    // ---- hardware
    perf_section(perf_l("HARDWARE", "HARDWARE"));
    if (sys.gpu_util >= 0 || sys.gpu_temp >= 0) {
        if (sys.gpu_util >= 0) snprintf(a, sizeof a, "%.0f%%", sys.gpu_util); else snprintf(a, sizeof a, "--");
        b[0] = 0;
        size_t k = 0;
        if (sys.gpu_temp >= 0) k += snprintf(b + k, sizeof b - k, "%.0f\xC2\xB0" "C  ", sys.gpu_temp);
        if (sys.gpu_watts >= 0 && k < sizeof b) k += snprintf(b + k, sizeof b - k, "%.0f W  ", sys.gpu_watts);
        if (sys.gpu_mhz >= 0 && k < sizeof b) snprintf(b + k, sizeof b - k, "%.0f MHz", sys.gpu_mhz);
        perf_row("GPU", a, b, sys.gpu_util >= 0 ? sys.gpu_util / 100.0f : -1, PERF_TEAL, s);
    }
    {   // VRAM: the whole GPU (telemetry) or the game's own (VMA), of the device-local heaps
        static VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
        static uint64_t budget_at;
        uint64_t now = SDL_GetTicksNS();
        if (g_vk.vma && (!budget_at || now - budget_at >= 500000000ull)) { vmaGetHeapBudgets(g_vk.vma, budgets); budget_at = now; }
        double game = 0, heap = 0;
        for (uint32_t i = 0; i < g_vk.memory_props.memoryHeapCount; i++)
            if (g_vk.memory_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                game += (double)budgets[i].usage;
                heap += (double)g_vk.memory_props.memoryHeaps[i].size;
            }
        double used = sys.vram_used >= 0 ? sys.vram_used : game, total = sys.vram_total > 0 ? sys.vram_total : heap;
        char u[24], t[24], g[24];
        perf_fmt_gb(u, sizeof u, used); perf_fmt_gb(t, sizeof t, total); perf_fmt_gb(g, sizeof g, game);
        snprintf(a, sizeof a, "%s / %s", u, t);
        snprintf(b, sizeof b, sys.vram_used >= 0 ? perf_l("game %s", "juego %s") : "%s", sys.vram_used >= 0 ? g : "");
        if (total > 0) perf_row("VRAM", a, sys.vram_used >= 0 ? b : "", (float)(used / total), PERF_TEAL, s);
    }
    if (sys.cpu >= 0) {
        snprintf(a, sizeof a, "%.0f%%", sys.cpu);
        if (sys.cpu_game >= 0) snprintf(b, sizeof b, perf_l("game %.0f%%", "juego %.0f%%"), sys.cpu_game); else b[0] = 0;
        perf_row("CPU", a, b, sys.cpu / 100.0f, PERF_TEAL, s);
    }
    if (sys.ram_total > 0) {
        char u[24], t[24], g[24];
        perf_fmt_gb(u, sizeof u, sys.ram_used); perf_fmt_gb(t, sizeof t, sys.ram_total);
        snprintf(a, sizeof a, "%s / %s", u, t);
        if (sys.ram_game >= 0) { perf_fmt_gb(g, sizeof g, sys.ram_game); snprintf(b, sizeof b, perf_l("game %s", "juego %s"), g); } else b[0] = 0;
        perf_row("RAM", a, b, (float)(sys.ram_used / sys.ram_total), PERF_TEAL, s);
    }

    // ---- the render path
    perf_section(perf_l("RENDER", "RENDER"));
    snprintf(a, sizeof a, "%ux%u", win_w, win_h);
    perf_row(perf_l("Window", "Ventana"), a, "", -1, PERF_TEAL, s);
    if (perf_game_w > 0) {
        snprintf(a, sizeof a, "%dx%d", perf_game_w, perf_game_h);
        perf_row(perf_l("Game image", "Imagen del juego"), a, "", -1, PERF_TEAL, s);
    }
    {
        uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
        const char *tmp = tmp_overlay_info(&rw, &rh, &ow, &oh);
        const char *ups;
        double pct;
        if (tmp) {
            static const char *q_en[] = {"Native AA", "Quality", "Balanced", "Performance", "Ultra perf."};
            static const char *q_es[] = {"AA nativo", "Calidad", "Equilibrado", "Rendimiento", "Ultra rend."};
            int qi = g_settings.upscale_quality >= 0 && g_settings.upscale_quality <= 4 ? g_settings.upscale_quality : 0;
            snprintf(b, sizeof b, "%s %s", tmp, perf_l(q_en[qi], q_es[qi]));
            ups = b;
            pct = ow ? 100.0 * rw / ow : 0;
        } else {
            rw = (uint32_t)std::max(0, perf_game_w); rh = (uint32_t)std::max(0, perf_game_h);
            pct = win_w && win_h ? 100.0 * std::max((double)rw / win_w, (double)rh / win_h) : 0;
            ups = pct >= 99.5 ? perf_l("Native", "Nativo")
                : g_settings.upscaler == UPSCALE_FSR1 ? "AMD FSR 1.0" : perf_l("Bilinear", "Bilineal");
        }
        if (rw) {
            snprintf(a, sizeof a, "%ux%u", rw, rh);
            char p[24]; snprintf(p, sizeof p, "%.0f%%", pct);
            perf_row(perf_l("3D scene", "Escena 3D"), a, p, -1, PERF_TEAL, s);
        }
        perf_row(perf_l("Upscaling", "Escalado"), ups, g_settings.antialias == AA_SMAA ? "+ SMAA" : "", -1, PERF_TEAL, s);
    }
    {
        if (g_settings.fps > 0) snprintf(a, sizeof a, "%d FPS", g_settings.fps); else snprintf(a, sizeof a, "%s", perf_l("Unlimited", "Sin límite"));
        const char *cut = g_settings.cutscene_mode == 1 ? perf_l("cutscenes 30", "escenas 30")
                        : g_settings.cutscene_mode == 2 ? perf_l("cutscenes 30 + FG", "escenas 30 + FG") : "";
        perf_row(perf_l("Frame limit", "Límite"), a, cut, -1, PERF_TEAL, s);
        float hz = vk_display_hz_cached();
        b[0] = 0;
        if (hz > 0) snprintf(b, sizeof b, "%.0f Hz%s", hz, tmp_fg_active() ? " + FG" : "");
        else if (tmp_fg_active()) snprintf(b, sizeof b, "+ FG");
        perf_row(perf_l("Presentation", "Presentación"), vk_present_mode_name(), b, -1, PERF_TEAL, s);
    }

    // ---- footer: the hardware names
    ImGui::Dummy(ImVec2(0, 2 * s));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.37f, 0.83f, 0.87f, 0.15f));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", g_vk.props.deviceName);
    if (perf_cpu_name[0]) ImGui::TextDisabled("%s", perf_cpu_name);
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

// Render thread, inside the ImGui frame.
static void perf_overlay_draw(bool shown, float s, uint32_t win_w, uint32_t win_h) {
    bool detailed = shown && g_settings.fps_overlay_style != FPS_OVERLAY_SIMPLE;
    vk_overlay_gpu_timing(detailed);
    perf_sys_wanted.store(detailed);
    if (!shown) return;
    perf_update_view(SDL_GetTicksNS(), detailed);
    int target = perf_target_fps();
    if (detailed) perf_draw_detailed(s, target, win_w, win_h);
    else perf_draw_simple(s, target);
}
