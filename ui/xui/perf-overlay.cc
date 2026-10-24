//
// xemu User Interface: performance overlay
//
// A small readout over the game, like the Steam performance overlay: the
// game's frame rate and 1% lows, the frame time and a graph of the last
// frames (spikes in orange and red), and how busy the PC is: xemu's CPU use
// and the whole PC's, the graphics card, xemu's memory and video memory.
// Turned on and set up in View > Performance Overlay.
//
// The frame times come from the game's own frames (each flip), not from the
// window, so they show what the player sees: a hitch in the game is a
// spike here even when the window keeps drawing.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "common.hh"
#include "perf-overlay.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "hw/xbox/nv2a/shader-precache.h"
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi1_4.h>
#endif

// Frames looked at: the 1% low covers the last LOW_SECONDS
#define MAX_FRAMES 1024
#define LOW_SECONDS 10

// The numbers change this often, so they can be read
#define REFRESH_SECONDS 0.5

// Frames in the graph
#define GRAPH_FRAMES 100

//
// PC load, read once a second on a thread of its own: the GPU counters can
// take a few milliseconds to read, which would be a hitch on the UI thread.
// -1 means not known (yet).
//
static std::atomic<bool> load_wanted{ false };
static std::atomic<bool> load_running{ false };
static std::atomic<int> load_cpu{ -1 }, load_pc_cpu{ -1 }, load_gpu{ -1 };
static std::atomic<int> load_ram_mb{ -1 }, load_vram_mb{ -1 };

#ifdef _WIN32
static ULONGLONG FileTime(FILETIME ft)
{
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// The graphics card with the most video memory: the one games run on
static IDXGIAdapter3 *FindAdapter()
{
    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  (void **)&factory))) {
        return NULL;
    }
    IDXGIAdapter3 *best = NULL;
    SIZE_T best_mem = 0;
    IDXGIAdapter1 *adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
         i++) {
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter3 *a3 = NULL;
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            desc.DedicatedVideoMemory > best_mem &&
            SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3),
                                              (void **)&a3))) {
            if (best) {
                best->Release();
            }
            best = a3;
            best_mem = desc.DedicatedVideoMemory;
        }
        adapter->Release();
    }
    factory->Release();
    return best;
}

static void LoadThread()
{
    IDXGIAdapter3 *adapter = FindAdapter();

    // "luid_0x00000000_0x0000D1A2": the GPU counters of that card only
    wchar_t luid[64] = L"";
    if (adapter) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            _snwprintf(luid, 63, L"luid_0x%08X_0x%08X",
                     (unsigned)desc.AdapterLuid.HighPart,
                     (unsigned)desc.AdapterLuid.LowPart);
        }
    }

    // How busy the 3D engine is, per process: added up, the whole card
    PDH_HQUERY query = NULL;
    PDH_HCOUNTER gpu_counter = NULL;
    if (PdhOpenQueryW(NULL, 0, &query) != ERROR_SUCCESS) {
        query = NULL;
    } else if (PdhAddEnglishCounterW(
                   query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage",
                   0, &gpu_counter) != ERROR_SUCCESS) {
        gpu_counter = NULL;
    } else {
        PdhCollectQueryData(query);
    }
    std::vector<BYTE> buf;

    ULONGLONG last_proc = 0, last_busy = 0, last_total = 0, last_wall = 0;
    double cpus = (std::max)(1u, std::thread::hardware_concurrency());

    while (load_wanted) {
        FILETIME c, e, k, u, idle, kern, user, now;
        GetSystemTimeAsFileTime(&now);
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) &&
            GetSystemTimes(&idle, &kern, &user)) {
            ULONGLONG proc = FileTime(k) + FileTime(u);
            ULONGLONG total = FileTime(kern) + FileTime(user);
            ULONGLONG busy = total - FileTime(idle);
            ULONGLONG wall = FileTime(now);
            if (last_wall && wall > last_wall && total > last_total) {
                load_cpu = (int)(100.0 * (proc - last_proc) /
                                 ((wall - last_wall) * cpus) + 0.5);
                load_pc_cpu = (int)(100.0 * (busy - last_busy) /
                                    (total - last_total) + 0.5);
            }
            last_proc = proc;
            last_busy = busy;
            last_total = total;
            last_wall = wall;
        }

        PROCESS_MEMORY_COUNTERS mem = {};
        mem.cb = sizeof(mem);
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &mem, sizeof(mem))) {
            load_ram_mb = (int)(mem.WorkingSetSize >> 20);
        }

        if (adapter) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info;
            if (SUCCEEDED(adapter->QueryVideoMemoryInfo(
                    0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                load_vram_mb = (int)(info.CurrentUsage >> 20);
            }
        }

        if (gpu_counter && PdhCollectQueryData(query) == ERROR_SUCCESS) {
            DWORD size = 0, count = 0;
            if (PdhGetFormattedCounterArrayW(gpu_counter, PDH_FMT_DOUBLE,
                                             &size, &count, NULL) ==
                (PDH_STATUS)PDH_MORE_DATA) {
                buf.resize(size);
                auto *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)buf.data();
                if (PdhGetFormattedCounterArrayW(gpu_counter, PDH_FMT_DOUBLE,
                                                 &size, &count, items) ==
                    ERROR_SUCCESS) {
                    double sum = 0;
                    for (DWORD i = 0; i < count; i++) {
                        if ((!luid[0] || wcsstr(items[i].szName, luid)) &&
                            (items[i].FmtValue.CStatus ==
                                 PDH_CSTATUS_VALID_DATA ||
                             items[i].FmtValue.CStatus ==
                                 PDH_CSTATUS_NEW_DATA)) {
                            sum += items[i].FmtValue.doubleValue;
                        }
                    }
                    load_gpu = (int)(std::min)(100.0, sum + 0.5);
                }
            }
        }

        Sleep(1000);
    }

    if (query) {
        PdhCloseQuery(query);
    }
    if (adapter) {
        adapter->Release();
    }
    load_running = false;
}
#endif

static void UpdateLoadThread(bool wanted)
{
    load_wanted = wanted;
#ifdef _WIN32
    if (wanted && !load_running) {
        load_running = true;
        std::thread(LoadThread).detach();
    }
#endif
}

//
// The numbers, worked out from the frame times
//
struct FrameStats {
    float fps = 0;       // frames in the last second
    float frame_ms = 0;  // average frame time in the last second
    float low = 0;       // 1% low: the frame rate of the slowest 1% of frames
    float pace_ms = 0;   // the game's usual frame time (median of the gaps)
    bool any = false;
};

static FrameStats Measure(const int64_t *times, int n, int64_t now)
{
    FrameStats s;
    if (n < 2) {
        return s;
    }
    s.any = true;

    // Frames in the last second
    int k = 0;
    while (k < n - 1 && now - times[n - 2 - k] <= 1000000) {
        k++;
    }
    if (k > 0 && now - times[n - 1] < 1000000) {
        double span = (times[n - 1] - times[n - 1 - k]) / 1e6;
        s.fps = span > 0 ? k / span : 0;
        s.frame_ms = k ? (float)(span * 1000.0 / k) : 0;
    }

    // Gaps of the last LOW_SECONDS, slowest first
    static std::vector<int64_t> gaps;
    gaps.clear();
    for (int i = n - 1; i > 0 && now - times[i] <= LOW_SECONDS * 1000000LL;
         i--) {
        gaps.push_back(times[i] - times[i - 1]);
    }
    if (!gaps.empty()) {
        std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2,
                         gaps.end());
        s.pace_ms = gaps[gaps.size() / 2] / 1000.0f;
        size_t worst = (std::max<size_t>)(1, gaps.size() / 100);
        std::partial_sort(gaps.begin(), gaps.begin() + worst, gaps.end(),
                          std::greater<int64_t>());
        double sum = 0;
        for (size_t i = 0; i < worst; i++) {
            sum += gaps[i];
        }
        s.low = sum > 0 ? (float)(1e6 * worst / sum) : 0;
    }
    return s;
}

// What the game aims for: 60 fps, or 30 fps for games like RS3 that are
// made to run at 30, so a steady 30 shows as good instead of as a problem
static float TargetFps(const FrameStats &s)
{
    return s.pace_ms > 0 && s.pace_ms <= 22.0f ? 60.0f : 30.0f;
}

// A frame against the game's own pace: green when on time, then yellow,
// orange, and red for a hitch
static ImU32 FrameColor(float ms, float pace, float alpha)
{
    int a = (int)(255 * alpha);
    if (ms <= pace * 1.25f) {
        return IM_COL32(110, 210, 90, a);
    } else if (ms <= pace * 1.7f) {
        return IM_COL32(230, 210, 70, a);
    } else if (ms <= pace * 2.6f) {
        return IM_COL32(240, 140, 40, a);
    }
    return IM_COL32(235, 70, 60, a);
}

static const ImVec4 good_color(0.50f, 0.88f, 0.40f, 1.0f);
static const ImVec4 fair_color(0.95f, 0.85f, 0.35f, 1.0f);
static const ImVec4 poor_color(0.98f, 0.40f, 0.35f, 1.0f);

static ImVec4 FpsColor(float fps, float target)
{
    return fps >= target * 0.9f ? good_color :
           fps >= target * 0.6f ? fair_color : poor_color;
}

// The 1% low is always under the frame rate, so it gets more room
static ImVec4 LowColor(float low, float target)
{
    return low >= target * 0.75f ? good_color :
           low >= target * 0.45f ? fair_color : poor_color;
}

//
// Drawing. Text goes straight to the window's draw list, so the Outline and
// Minimal styles can put a dark edge around it that keeps it readable on
// any picture without a background.
//
struct Look {
    bool outline;        // dark edge around text, no panel
    float small, big;    // text sizes in pixels
    ImFont *font, *big_font;
};

static Look look;

static const ImVec4 label_color(0.62f, 0.70f, 0.78f, 1.0f);
static const ImVec4 value_color(1.0f, 1.0f, 1.0f, 1.0f);
static const ImVec4 dim_color(0.62f, 0.62f, 0.62f, 1.0f);

static void TextAt(ImVec2 p, ImFont *font, float size, const ImVec4 &color,
                   const char *text, bool edge_always = false)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (look.outline || edge_always) {
        float o = (std::max)(1.0f, size / 14.0f);
        ImU32 edge = IM_COL32(0, 0, 0, 210);
        for (float dx = -o; dx <= o; dx += o) {
            for (float dy = -o; dy <= o; dy += o) {
                if (dx != 0 || dy != 0) {
                    dl->AddText(font, size, ImVec2(p.x + dx, p.y + dy), edge,
                                text);
                }
            }
        }
    }
    dl->AddText(font, size, p, ImGui::GetColorU32(color), text);
}

// Text at the cursor; returns its width
static float Txt(ImFont *font, float size, const ImVec4 &color,
                 const char *fmt, ...) IM_FMTARGS(4);
static float Txt(ImFont *font, float size, const ImVec4 &color,
                 const char *fmt, ...)
{
    char text[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    TextAt(ImGui::GetCursorScreenPos(), font, size, color, text);
    ImGui::Dummy(sz);
    return sz.x;
}

static float Width(ImFont *font, float size, const char *text)
{
    return font->CalcTextSizeA(size, FLT_MAX, 0, text).x;
}

// How far below the top of a line of text its baseline sits
static float Ascent(ImFont *font, float size)
{
    return font->Ascent * size / font->FontSize;
}

// A small label then its value on the same line, both at the small size
static void Pair(const char *label, const ImVec4 &color, const char *value)
{
    Txt(look.font, look.small, label_color, "%s", label);
    ImGui::SameLine(0, look.small * 0.3f);
    Txt(look.font, look.small, color, "%s", value);
}

// The last frames as bars, a line at the frame time the game aims for, and
// the average frame time in the top right corner. Steady frames fill a third
// of the height, so a hitch stands out above them.
static void DrawGraph(const int64_t *times, int n, int64_t now, int frames,
                      float width, float height, const FrameStats &stats,
                      const char *caption)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float target_ms = 1000.0f / TargetFps(stats);
    float pace = stats.pace_ms > 0 ? stats.pace_ms : target_ms;
    float scale = height / (std::max)(target_ms * 3.0f, 50.0f);

    if (!look.outline) {
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height),
                          IM_COL32(0, 0, 0, 70), 3.0f);
    }
    float line_y = p.y + height - target_ms * scale;
    dl->AddLine(ImVec2(p.x, line_y), ImVec2(p.x + width, line_y),
                IM_COL32(255, 255, 255, look.outline ? 70 : 45));

    // Newest on the right. The frame being waited for grows as it waits,
    // so a freeze shows as a red bar.
    static std::vector<float> bars;
    bars.clear();
    if (n > 0 && now - times[n - 1] > 20000) {
        bars.push_back((now - times[n - 1]) / 1000.0f);
    }
    for (int j = n - 1; j >= 1 && (int)bars.size() < frames; j--) {
        bars.push_back((times[j] - times[j - 1]) / 1000.0f);
    }
    float bar = width / frames;
    for (size_t i = 0; i < bars.size(); i++) {
        float ms = bars[i];
        float x1 = p.x + width - i * bar;
        float h = (std::min)(height, (std::max)(1.0f, ms * scale));
        dl->AddRectFilled(ImVec2(x1 - bar + (bar > 3 ? 1 : 0),
                                 p.y + height - h),
                          ImVec2(x1, p.y + height),
                          FrameColor(ms, pace, look.outline ? 0.9f : 1.0f));
    }
    if (caption) {
        float size = look.small * 0.8f;
        float pad = look.small * 0.2f;
        TextAt(ImVec2(p.x + width - pad - Width(look.font, size, caption),
                      p.y + pad * 0.5f),
               look.font, size, value_color, caption, true);
    }
    ImGui::Dummy(ImVec2(width, height));
}

static void FormatMemory(char *out, size_t size, int mb)
{
    if (mb >= 1024) {
        snprintf(out, size, "%.1f GB", mb / 1024.0);
    } else {
        snprintf(out, size, "%d MB", mb);
    }
}

// What there is to show, with the numbers of the last refresh
struct Shown {
    bool fps, lows, frame_time, graph, shaders;
    bool paused;
    FrameStats stats;
    int cpu, pc_cpu, gpu, ram, vram;   // -1 when hidden or not known
    ShaderPrecacheStatus shader_status;
};

// The big frame rate with "FPS" on its baseline, and the 1% low on the same
// baseline at the right edge of width
static void BigFps(const Shown &s, bool lows, float width)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float small_y = p.y + Ascent(look.big_font, look.big) -
                    Ascent(look.font, look.small);
    float target = TargetFps(s.stats);
    float x = p.x;
    char v[16];

    if (s.paused) {
        TextAt(p, look.big_font, look.big, dim_color, "PAUSED");
        x += Width(look.big_font, look.big, "PAUSED");
    } else {
        snprintf(v, sizeof(v), "%.0f", s.stats.fps);
        TextAt(p, look.big_font, look.big, FpsColor(s.stats.fps, target), v);
        x += Width(look.big_font, look.big, v) + look.small * 0.3f;
        TextAt(ImVec2(x, small_y), look.font, look.small, label_color, "FPS");
        x += Width(look.font, look.small, "FPS");
    }
    if (lows && s.stats.any) {
        snprintf(v, sizeof(v), "%.0f", s.stats.low);
        float gap = look.small * 0.3f;
        float label_w = Width(look.font, look.small, "1% LOW");
        float value_w = Width(look.font, look.small, v);
        float right = (std::max)(p.x + width, x + look.small * 0.9f + label_w +
                                              gap + value_w);
        TextAt(ImVec2(right - value_w, small_y), look.font, look.small,
               LowColor(s.stats.low, target), v);
        TextAt(ImVec2(right - value_w - gap - label_w, small_y), look.font,
               look.small, label_color, "1% LOW");
        x = right;
    }
    ImGui::Dummy(ImVec2(x - p.x, look.big));
}

static void DrawDetailed(const Shown &s, const int64_t *times, int n,
                         int64_t now)
{
    float width = look.small * 11.0f;
    bool any = false;
    char v[32], caption[32];

    if (s.fps) {
        BigFps(s, s.lows, width);
        any = true;
    } else if (s.lows && s.stats.any) {
        snprintf(v, sizeof(v), "%.0f", s.stats.low);
        Pair("1% LOW", LowColor(s.stats.low, TargetFps(s.stats)), v);
        any = true;
    }

    snprintf(caption, sizeof(caption), "%.1f ms", s.stats.frame_ms);
    bool caption_shown = s.frame_time && !s.paused && s.stats.any;
    if (s.graph) {
        DrawGraph(times, n, now, GRAPH_FRAMES, width, look.small * 2.4f,
                  s.stats, caption_shown ? caption : NULL);
        any = true;
    } else if (caption_shown) {
        Pair("FRAME", value_color, caption);
        any = true;
    }

    // Two columns: CPU GPU, then RAM VRAM
    float col = ImGui::GetCursorPosX() + width * 0.5f;
    int cells = 0;
    auto cell = [&](const char *label, const char *value) {
        if (cells % 2) {
            ImGui::SameLine(col);
        }
        Pair(label, value_color, value);
        cells++;
    };
    if (s.cpu >= 0) {
        snprintf(v, sizeof(v), "%d%%", s.cpu);
        cell("CPU", v);
    }
    if (s.gpu >= 0) {
        snprintf(v, sizeof(v), "%d%%", s.gpu);
        cell("GPU", v);
    }
    if (s.ram >= 0) {
        FormatMemory(v, sizeof(v), s.ram);
        cell("RAM", v);
    }
    if (s.vram >= 0) {
        FormatMemory(v, sizeof(v), s.vram);
        cell("VRAM", v);
    }
    any |= cells > 0;

    if (s.shaders) {
        snprintf(v, sizeof(v), "%d/%d",
                 s.shader_status.done + s.shader_status.failed,
                 s.shader_status.list_missing);
        Pair("SHADERS", value_color, v);
        any = true;
    }
    if (!any) {
        Txt(look.font, look.small, dim_color, "Nothing to show");
    }
}

// "60 FPS  1% LOW 54  16.7 ms  CPU 12%  GPU 38%  ..." on one line
static void DrawCompact(const Shown &s)
{
    float gap = look.small * 0.8f;
    bool first = true;
    char v[32];
    auto next = [&]() {
        if (!first) {
            ImGui::SameLine(0, gap);
        }
        first = false;
    };
    if (s.fps) {
        next();
        if (s.paused) {
            Txt(look.font, look.small, dim_color, "PAUSED");
        } else {
            snprintf(v, sizeof(v), "%.0f", s.stats.fps);
            Txt(look.font, look.small,
                FpsColor(s.stats.fps, TargetFps(s.stats)), "%s", v);
            ImGui::SameLine(0, look.small * 0.3f);
            Txt(look.font, look.small, label_color, "FPS");
        }
    }
    if (s.lows && s.stats.any) {
        next();
        snprintf(v, sizeof(v), "%.0f", s.stats.low);
        Pair("1% LOW", LowColor(s.stats.low, TargetFps(s.stats)), v);
    }
    if (s.frame_time && !s.paused && s.stats.any) {
        next();
        Txt(look.font, look.small, value_color, "%.1f ms", s.stats.frame_ms);
    }
    if (s.cpu >= 0) {
        next();
        snprintf(v, sizeof(v), "%d%%", s.cpu);
        Pair("CPU", value_color, v);
    }
    if (s.gpu >= 0) {
        next();
        snprintf(v, sizeof(v), "%d%%", s.gpu);
        Pair("GPU", value_color, v);
    }
    if (s.ram >= 0) {
        next();
        FormatMemory(v, sizeof(v), s.ram);
        Pair("RAM", value_color, v);
    }
    if (s.vram >= 0) {
        next();
        FormatMemory(v, sizeof(v), s.vram);
        Pair("VRAM", value_color, v);
    }
    if (s.shaders) {
        next();
        snprintf(v, sizeof(v), "%d/%d",
                 s.shader_status.done + s.shader_status.failed,
                 s.shader_status.list_missing);
        Pair("SHADERS", value_color, v);
    }
    if (first) {
        Txt(look.font, look.small, dim_color, "Nothing to show");
    }
}

static float top_right_bottom;

float PerfOverlayTopRightBottom()
{
    return top_right_bottom;
}

void DrawPerfOverlay()
{
    top_right_bottom = 0;
    auto &o = g_config.display.overlay;
    bool minimal = o.style == CONFIG_DISPLAY_OVERLAY_STYLE_MINIMAL;
    bool compact = o.style == CONFIG_DISPLAY_OVERLAY_STYLE_COMPACT;
    UpdateLoadThread(o.enabled && !minimal &&
                     (o.show_cpu || o.show_gpu || o.show_ram || o.show_vram));
    if (!o.enabled) {
        return;
    }

    static int64_t times[MAX_FRAMES];
    int64_t now;
    int n = nv2a_profile_get_frame_times(times, MAX_FRAMES, &now);

    static FrameStats stats;
    static double next_refresh;
    double t = ImGui::GetTime();
    if (t >= next_refresh) {
        stats = Measure(times, n, now);
        next_refresh = t + REFRESH_SECONDS;
    }

    Shown s;
    s.paused = !runstate_is_running();
    s.stats = stats;
    s.fps = o.show_fps || minimal;
    s.lows = o.show_lows;
    s.frame_time = o.show_frame_time;
    s.graph = o.show_graph;
    s.cpu = o.show_cpu ? load_cpu.load() : -1;
    s.pc_cpu = load_pc_cpu;
    s.gpu = o.show_gpu ? load_gpu.load() : -1;
    s.ram = o.show_ram ? load_ram_mb.load() : -1;
    s.vram = o.show_vram ? load_vram_mb.load() : -1;
    nv2a_shader_precache_get_status(&s.shader_status);
    s.shaders = o.show_shaders && s.shader_status.running;

    static const float sizes[] = { 0.8f, 1.0f, 1.3f };
    float fs = sizes[std::clamp(o.size, 0, 2)];
    // Below about a third the background no longer keeps the text readable
    // on a bright picture, so the text gets its dark edge
    int opacity = std::clamp(o.opacity, 0, 100);
    look.outline = opacity < 35;
    look.font = g_font_mgr.m_default_font;
    look.big_font = g_font_mgr.m_menu_font_medium;
    look.small = look.font->FontSize * fs;
    look.big = look.big_font->FontSize * fs * (minimal ? 1.25f : 1.0f);

    // Place it in the chosen corner
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float scale = g_viewport_mgr.m_scale;
    float pad = std::clamp(o.margin, 0, 200) * scale;
    bool right = o.position == CONFIG_DISPLAY_OVERLAY_POSITION_TOP_RIGHT ||
                 o.position == CONFIG_DISPLAY_OVERLAY_POSITION_BOTTOM_RIGHT;
    bool bottom = o.position == CONFIG_DISPLAY_OVERLAY_POSITION_BOTTOM_LEFT ||
                  o.position == CONFIG_DISPLAY_OVERLAY_POSITION_BOTTOM_RIGHT;
    ImVec2 pos(right ? vp->WorkPos.x + vp->WorkSize.x - pad :
                       vp->WorkPos.x + pad,
               bottom ? vp->WorkPos.y + vp->WorkSize.y - pad :
                        vp->WorkPos.y + pad);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always,
                            ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));

    // The padding stays the same with any background, so changing the
    // background never moves the numbers
    static const ImVec4 bg_colors[] = {
        ImVec4(0.07f, 0.08f, 0.09f, 1.0f),  // charcoal
        ImVec4(0.0f, 0.0f, 0.0f, 1.0f),     // black
        ImVec4(0.04f, 0.08f, 0.19f, 1.0f),  // navy
        ImVec4(0.20f, 0.23f, 0.27f, 1.0f),  // slate
    };
    ImGui::SetNextWindowBgAlpha(opacity / 100.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,
                        o.border ? (std::max)(1.0f, scale) : 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                        o.rounded ? look.small * 0.4f : 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(look.small * 0.6f, look.small * 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(look.small * 0.4f, look.small * 0.25f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          bg_colors[std::clamp(o.background_color, 0, 3)]);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.22f));

    if (ImGui::Begin("Performance Overlay", NULL,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        if (minimal) {
            BigFps(s, s.lows, 0);
        } else if (compact) {
            DrawCompact(s);
        } else {
            DrawDetailed(s, times, n, now);
        }
        if (right && !bottom) {
            top_right_bottom = ImGui::GetWindowPos().y +
                               ImGui::GetWindowSize().y;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}
