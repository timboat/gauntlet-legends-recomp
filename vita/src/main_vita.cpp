// Gauntlet Legends: Recompiled - PS Vita frontend (N64ModernRuntime on the recompiled game, own GXM renderer).
// Data lives in ux0:data/GauntletLegends: the ROM (rom.z64), saves/, log.txt and perf.txt.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/io/stat.h>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "xxHash/xxh3.h"
#include "lightweightsemaphore.h"

#include "gauntlet_game.h"
#include "vita_log.h"
#include "vita_render.h"

void vita_register_thread(const char* name);
void vu_check();

// Heap and thread stack sizes for newlib / pthread-embedded. Game threads run recompiled code on the host stack.
extern "C" {
    int _newlib_heap_size_user = 192 * 1024 * 1024;
    unsigned int _pthread_stack_default_user = 2 * 1024 * 1024;
}

static const std::u8string game_id = u8"gauntlet.n64.us";
constexpr const char* data_dir = "ux0:data/GauntletLegends";

// ---- Logging ----
// Vita3K only flushes guest files on fclose, so every line opens and closes the log.
static std::mutex log_mutex;
void vita_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard lock{ log_mutex };
    if (FILE* f = fopen("ux0:data/GauntletLegends/log.txt", "a")) {
        fputs(buf, f);
        fclose(f);
    }
}

[[noreturn]] static void exit_error(const char* msg) {
    vita_log("FATAL: %s\n", msg);
    sceKernelExitProcess(0);
    for (;;) {}
}

// ---- Stats (perf.txt every 5 s) ----
std::atomic<uint32_t> stat_dl_count{ 0 };
std::atomic<uint32_t> stat_vi_count{ 0 };
static std::atomic<uint32_t> stat_audio_frames{ 0 };
static std::atomic<uint32_t> stat_audio_underruns{ 0 };

// ---- Audio: ring of game-rate stereo frames, resampled to 48 kHz by an output thread ----
namespace {
    constexpr uint32_t out_rate = 48000;
    constexpr uint32_t out_grain = 256;
    constexpr size_t ring_frames = 16384;
    std::array<int16_t, ring_frames * 2> ring;
    std::atomic<size_t> ring_read{ 0 }, ring_write{ 0 };
    std::atomic<uint32_t> in_rate{ 22050 };

    size_t ring_fill() { return ring_write.load() - ring_read.load(); }

    void audio_thread_func() {
        vita_register_thread("Audio Out");
        int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, out_grain, out_rate, SCE_AUDIO_OUT_MODE_STEREO);
        if (port < 0) {
            vita_log("sceAudioOutOpenPort failed 0x%08X\n", port);
            return;
        }
        int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
        sceAudioOutSetVolume(port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
        static int16_t out[out_grain * 2];
        uint32_t frac = 0; // 16.16 position between ring frames
        while (true) {
            uint32_t step = (uint32_t)(((uint64_t)in_rate.load() << 16) / out_rate);
            size_t rd = ring_read.load();
            size_t avail = ring_write.load() - rd;
            size_t needed = (((uint64_t)frac + (uint64_t)step * out_grain) >> 16) + 2;
            if (avail < needed) {
                memset(out, 0, sizeof(out));
                if (ultramodern::is_game_started()) stat_audio_underruns++;
            } else {
                for (uint32_t i = 0; i < out_grain; i++) {
                    size_t idx = rd + (frac >> 16);
                    int32_t t = frac & 0xFFFF;
                    const int16_t* a = &ring[(idx % ring_frames) * 2];
                    const int16_t* b = &ring[((idx + 1) % ring_frames) * 2];
                    out[i * 2 + 0] = (int16_t)(a[0] + (((b[0] - a[0]) * t) >> 16));
                    out[i * 2 + 1] = (int16_t)(a[1] + (((b[1] - a[1]) * t) >> 16));
                    frac += step;
                }
                ring_read.store(rd + (frac >> 16));
                frac &= 0xFFFF;
            }
            sceAudioOutOutput(port, out);
        }
    }

    // Samples arrive as host-order 16-bit pairs with the channels swapped by the 32-bit word layout.
    void queue_samples(int16_t* audio_data, size_t sample_count) {
        size_t wr = ring_write.load();
        size_t frames = sample_count / 2;
        if (ring_fill() + frames > ring_frames) {
            return; // overflow: drop
        }
        for (size_t i = 0; i < frames; i++) {
            int16_t* dst = &ring[((wr + i) % ring_frames) * 2];
            dst[0] = audio_data[i * 2 + 1];
            dst[1] = audio_data[i * 2 + 0];
        }
        ring_write.store(wr + frames);
        stat_audio_frames += (uint32_t)frames;
    }

    size_t get_frames_remaining() {
        // Report what is still queued minus one VI, so the game stays a little ahead of the output.
        size_t fill = ring_fill();
        size_t vi = in_rate.load() / 60;
        return fill > vi ? fill - vi : 0;
    }

    void set_frequency(uint32_t freq) {
        in_rate.store(freq);
    }
}

// ---- Input ----
namespace {
    constexpr uint16_t N64_A = 0x8000, N64_B = 0x4000, N64_Z = 0x2000, N64_START = 0x1000;
    constexpr uint16_t N64_DU = 0x0800, N64_DD = 0x0400, N64_DL = 0x0200, N64_DR = 0x0100;
    constexpr uint16_t N64_L = 0x0020, N64_R = 0x0010;
    constexpr uint16_t N64_CU = 0x0008, N64_CD = 0x0004, N64_CL = 0x0002, N64_CR = 0x0001;

    float stick(uint8_t v) {
        float f = ((int)v - 128) / 127.0f;
        if (f > -0.15f && f < 0.15f) return 0.0f;
        return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
    }

    // Test aid: ux0:data/GauntletLegends/autoplay.txt with lines "<seconds> <hex buttons> [<ms held>]".
    struct AutoPress { double t; uint16_t buttons; double held; };
    std::vector<AutoPress> auto_presses;
    auto start_time = std::chrono::steady_clock::now();

    void load_autoplay() {
        FILE* f = fopen("ux0:data/GauntletLegends/autoplay.txt", "r");
        if (!f) return;
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            double t, held = 150; unsigned b;
            if (sscanf(line, "%lf %x %lf", &t, &b, &held) >= 2) auto_presses.push_back({ t, (uint16_t)b, held / 1000.0 });
        }
        fclose(f);
        vita_log("autoplay: %d presses\n", (int)auto_presses.size());
    }

    uint16_t auto_buttons() {
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        uint16_t b = 0;
        for (const auto& p : auto_presses) {
            if (now >= p.t && now < p.t + p.held) b |= p.buttons;
        }
        return b;
    }

    // × attack (A), □ magic (B), ○ / △ C-down / C-up, L = Z, R = R, d-pad, left stick moves, right stick = C buttons.
    bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
        if (controller_num != 0) {
            return false;
        }
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint16_t b = auto_buttons();
        auto map = [&](uint32_t vb, uint16_t nb) { if (pad.buttons & vb) b |= nb; };
        map(SCE_CTRL_CROSS, N64_A);
        map(SCE_CTRL_SQUARE, N64_B);
        map(SCE_CTRL_CIRCLE, N64_CD);
        map(SCE_CTRL_TRIANGLE, N64_CU);
        map(SCE_CTRL_START, N64_START);
        map(SCE_CTRL_SELECT, N64_L);
        map(SCE_CTRL_LTRIGGER, N64_Z);
        map(SCE_CTRL_RTRIGGER, N64_R);
        map(SCE_CTRL_UP, N64_DU);
        map(SCE_CTRL_DOWN, N64_DD);
        map(SCE_CTRL_LEFT, N64_DL);
        map(SCE_CTRL_RIGHT, N64_DR);
        float rx = stick(pad.rx), ry = stick(pad.ry);
        if (rx < -0.5f) b |= N64_CL;
        if (rx > 0.5f) b |= N64_CR;
        if (ry < -0.5f) b |= N64_CU;
        if (ry > 0.5f) b |= N64_CD;
        *buttons = b;
        *x = stick(pad.lx);
        *y = -stick(pad.ly);
        return true;
    }

    void poll_input() {}
    void set_rumble(int, bool) {}

    ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
        // One controller, holding the Controller Pak (data/saves/controller_pak_1.mpk).
        if (controller_num == 0) {
            return { ultramodern::input::Device::Controller, ultramodern::input::Pak::ControllerPak };
        }
        return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }
}

// ---- RSP ----
RspExitReason gauntlet_audio_ucode(uint8_t* rdram, uint32_t ucode_addr);

// Audio task statistics (perf.txt "rsp:" line): tasks, total and longest run time, and a sampled profile of the
// microcode by block address (audio_ucode.cpp sets rsp_prof_label; 0 = not in a task).
volatile uint16_t rsp_prof_label = 0;
static std::atomic<uint32_t> rsp_tasks{ 0 }, rsp_us{ 0 }, rsp_max_us{ 0 };
static std::atomic<uint32_t> rsp_prof_hist[0x1000];
static std::atomic<uint32_t> rsp_prof_total{ 0 };

static int rsp_prof_thread(SceSize, void*) {
    for (;;) {
        sceKernelDelayThread(250);
        uint16_t l = rsp_prof_label;
        if (l) rsp_prof_hist[l & 0xFFF]++;
        rsp_prof_total++;
    }
    return 0;
}

static RspExitReason timed_audio_ucode(uint8_t* rdram, uint32_t ucode_addr) {
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    RspExitReason r = gauntlet_audio_ucode(rdram, ucode_addr);
    rsp_prof_label = 0;
    uint32_t us = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    rsp_tasks++;
    rsp_us += us;
    if (us > rsp_max_us) rsp_max_us = us;
    return r;
}

static void rsp_stats_text(char* buf, size_t n, double secs) {
    uint32_t tasks = rsp_tasks.exchange(0), us = rsp_us.exchange(0), mx = rsp_max_us.exchange(0);
    uint32_t total = rsp_prof_total.exchange(0);
    int len = snprintf(buf, n, "%.1f tasks/s, %.2f ms avg, %.2f ms max, busy %.0f%%", tasks / secs,
        tasks ? us / 1000.0 / tasks : 0.0, mx / 1000.0, us / (secs * 1e4));
    // Top blocks by samples.
    uint32_t top[8] = {}, topc[8] = {};
    for (uint32_t a = 0; a < 0x1000; a++) {
        uint32_t c = rsp_prof_hist[a].exchange(0);
        for (int k = 0; k < 8; k++)
            if (c > topc[k]) {
                for (int j = 7; j > k; j--) { topc[j] = topc[j - 1]; top[j] = top[j - 1]; }
                topc[k] = c; top[k] = a; break;
            }
    }
    if (total) for (int k = 0; k < 8 && topc[k] && len > 0 && (size_t)len < n; k++)
        len += snprintf(buf + len, n - len, " %s%03X %.1f%%", k ? "" : "| ", top[k] | 0x1000, topc[k] * 100.0 / total);
}

static RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    if (task->t.type == M_AUDTASK) {
        return timed_audio_ucode;
    }
    vita_log("Unknown RSP task type: %u\n", (unsigned)task->t.type);
    return nullptr;
}

// ---- Frontend callbacks ----
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

static ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    return {};
}

void vita_render_stats(char* buf, size_t n);
extern std::atomic<uint32_t> ultramodern_dropped_messages, ultramodern_dropped_last_mq, ultramodern_dropped_last_msg;
extern std::atomic<uint32_t> ultramodern_skipped_presents;
extern std::atomic<uint32_t> ultramodern_gfx_yields;

// Per-thread CPU time (perf.txt "cpu:" line): threads register by name; each report shows the share of the 5 s
// each one ran.
namespace {
    struct ThreadRec { char name[24]; SceUID id; uint64_t last; };
    std::mutex thread_recs_mutex;
    std::vector<ThreadRec> thread_recs;
}

void vita_register_thread(const char* name) {
    std::lock_guard<std::mutex> lock(thread_recs_mutex);
    ThreadRec r{};
    snprintf(r.name, sizeof(r.name), "%s", name);
    r.id = sceKernelGetThreadId();
    thread_recs.push_back(r);
}

static void thread_cpu_text(char* buf, size_t n, double secs) {
    std::lock_guard<std::mutex> lock(thread_recs_mutex);
    size_t len = 0;
    buf[0] = 0;
    for (auto it = thread_recs.begin(); it != thread_recs.end();) {
        SceKernelThreadInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(it->id, &info) < 0) { it = thread_recs.erase(it); continue; }
        uint64_t d = info.runClocks - it->last;
        it->last = info.runClocks;
        double pct = d / (secs * 1e4);
        if (pct >= 1.0 && len + 40 < n) len += snprintf(buf + len, n - len, " %s %.0f%%", it->name, pct);
        ++it;
    }
    if (len == 0) snprintf(buf, n, " (%u threads, no run times)", (unsigned)thread_recs.size());
}

static void update_gfx(void*) {
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - last).count();
    if (secs >= 5.0) {
        if (FILE* f = fopen("ux0:data/GauntletLegends/perf.txt", "a")) {
            fprintf(f, "dl/s %.1f  vi/s %.1f  audio frames/s %.0f  audio underruns %u\n",
                stat_dl_count.exchange(0) / secs, stat_vi_count.exchange(0) / secs,
                stat_audio_frames.exchange(0) / secs, (unsigned)stat_audio_underruns.exchange(0));
            char rs[900];
            vita_render_stats(rs, sizeof(rs));
            fprintf(f, "  gfx: %s\n", rs);
            char tc[400];
            thread_cpu_text(tc, sizeof(tc), secs);
            char rsp_text[300];
            rsp_stats_text(rsp_text, sizeof(rsp_text), secs);
            fprintf(f, "  rsp: %s\n", rsp_text);
            fprintf(f, "  cpu:%s\n", tc);
            {
                static uint32_t last_yields = 0;
                uint32_t y = ultramodern_gfx_yields.load();
                fprintf(f, "  gfx yields for audio: %.1f/s\n", (y - last_yields) / secs);
                last_yields = y;
            }
            if (uint32_t k = ultramodern_skipped_presents.load())
                fprintf(f, "  skipped stale presents: %u\n", k);
            if (uint32_t d = ultramodern_dropped_messages.load())
                fprintf(f, "  dropped interrupt messages: %u (last queue %08X msg %08X)\n", d,
                    ultramodern_dropped_last_mq.load(), ultramodern_dropped_last_msg.load());
            fclose(f);
        }
        last = now;
    }
}

static void message_box(const char* msg) {
    vita_log("%s\n", msg);
}

// Start-up check that thread_local storage is really per thread (the runtime relies on it).
static thread_local int tls_probe = 0;
static void check_tls() {
    tls_probe = 1;
    int seen = -1;
    std::thread t{ [&] { seen = tls_probe; tls_probe = 2; } };
    t.join();
    vita_log("TLS check: other thread saw %d (want 0), main now %d (want 1)\n", seen, tls_probe);

    // Mutex: 4 threads x 100k increments.
    std::mutex m;
    int counter = 0;
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; i++) ts.emplace_back([&] { for (int j = 0; j < 100000; j++) { std::lock_guard l{ m }; counter++; } });
    for (auto& th : ts) th.join();
    vita_log("mutex check: %d (want 400000)\n", counter);

    // Semaphore hand-off, as the scheduler uses it: two threads must never be inside at once.
    moodycamel::LightweightSemaphore a, b;
    std::atomic<int> inside{ 0 }, overlaps{ 0 }, rounds{ 0 };
    std::thread other{ [&] {
        for (int i = 0; i < 20000; i++) {
            a.wait();
            if (inside.fetch_add(1) != 0) overlaps++;
            rounds++;
            inside.fetch_sub(1);
            b.signal();
        }
    } };
    for (int i = 0; i < 20000; i++) {
        if (inside.fetch_add(1) != 0) overlaps++;
        inside.fetch_sub(1);
        a.signal();
        b.wait();
    }
    other.join();
    vita_log("semaphore check: %d rounds (want 20000), %d overlaps (want 0)\n", rounds.load(), overlaps.load());
}

static bool load_rom() {
    const char* names[] = { "ux0:data/GauntletLegends/rom.z64", "ux0:data/GauntletLegends/Gauntlet Legends (USA).z64" };
    for (const char* name : names) {
        FILE* f = fopen(name, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> rom(size);
        size_t got = fread(rom.data(), 1, size, f);
        fclose(f);
        if (got != (size_t)size) continue;
        uint64_t hash = XXH3_64bits(rom.data(), rom.size());
        if (hash != 0x88845DB2CBC34EE7ULL) {
            vita_log("%s: wrong ROM (hash %016llX); expected the USA .z64\n", name, (unsigned long long)hash);
            continue;
        }
        recomp::set_rom_contents(std::move(rom));
        vita_log("ROM loaded from %s\n", name);
        return true;
    }
    return false;
}

int main(int argc, char** argv) {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    sceIoMkdir(data_dir, 0777);
    sceIoMkdir("ux0:data/GauntletLegends/saves", 0777);
    vita_log("---- Gauntlet Legends: Recompiled (Vita) ----\n");
    check_tls();
    vu_check();

    if (!load_rom()) {
        exit_error("No ROM: put the USA Gauntlet Legends .z64 at ux0:data/GauntletLegends/rom.z64");
    }
    load_autoplay();

    recomp::register_config_path(data_dir);

    recomp::GameEntry game{};
    game.rom_hash = 0x88845DB2CBC34EE7ULL;
    game.internal_name = "GAUNTLET LEGENDS";
    game.game_id = game_id;
    game.mod_game_id = "";
    game.save_type = recomp::SaveType::AllowAll;
    game.is_enabled = true;
    game.has_compressed_code = false;
    game.entrypoint_address = (gpr)gauntlet::entrypoint;
    game.entrypoint = recomp_entrypoint;
    game.on_init_callback = gauntlet::on_init;
    recomp::register_game(game);
    gauntlet::register_overlays();

    std::thread{ audio_thread_func }.detach();

    recomp::rsp::callbacks_t rsp_callbacks{ .get_rsp_microcode = get_rsp_microcode };
    ultramodern::renderer::callbacks_t renderer_callbacks{ .create_render_context = gauntlet::vita::create_render_context };
    ultramodern::gfx_callbacks_t gfx_callbacks{ .create_gfx = create_gfx, .create_window = nullptr, .update_gfx = update_gfx };
    ultramodern::audio_callbacks_t audio_callbacks{ .queue_samples = queue_samples, .get_frames_remaining = get_frames_remaining, .set_frequency = set_frequency };
    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = poll_input,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };
    ultramodern::events::callbacks_t events_callbacks{ .vi_callback = [] { stat_vi_count++; } };
    ultramodern::error_handling::callbacks_t error_handling_callbacks{ .message_box = message_box };
    ultramodern::threads::callbacks_t threads_callbacks{};

    extern bool ultramodern_early_sp_complete;
    // Test switch: ux0:data/GauntletLegends/overlap.txt sends SP complete before rendering (game/renderer overlap).
    if (FILE* f = fopen("ux0:data/GauntletLegends/overlap.txt", "rb")) { fclose(f); ultramodern_early_sp_complete = true; }
    vita_log("game/renderer overlap: %s\n", ultramodern_early_sp_complete ? "on" : "off");
    SceUID rsp_prof = sceKernelCreateThread("rsp_prof", rsp_prof_thread, 0x10000100, 0x1000, 0, 0, nullptr);
    if (rsp_prof >= 0) sceKernelStartThread(rsp_prof, 0, nullptr);
    recomp::start_game(game_id);
    recomp::start({ 0, 1, 0 }, {}, rsp_callbacks, renderer_callbacks, audio_callbacks, input_callbacks,
        gfx_callbacks, events_callbacks, error_handling_callbacks, threads_callbacks);

    sceKernelExitProcess(0);
    return 0;
}

#ifdef ULTRAMODERN_DEBUG_PRINTF
// Runtime trace (debug builds): a lock-free ring of (format, raw int arguments, host thread), formatted only when
// written to trace.txt on abort, so tracing barely changes the timing it is meant to observe.
#include <exception>
#include <pthread.h>
namespace {
    struct TraceEntry { const char* fmt; uint32_t args[4]; uint32_t thread; };
    constexpr size_t trace_len = 1 << 16;
    TraceEntry trace_ring[trace_len];
    std::atomic<uint32_t> trace_next{ 0 };

    void dump_trace() {
        FILE* f = fopen("ux0:data/GauntletLegends/trace.txt", "wb");
        if (!f) return;
        uint32_t end = trace_next.load();
        uint32_t start = end > trace_len ? end - trace_len : 0;
        for (uint32_t i = start; i < end; i++) {
            const TraceEntry& e = trace_ring[i % trace_len];
            if (!e.fmt || strstr(e.fmt, "%s") || strstr(e.fmt, "%p")) continue;
            char line[256];
            snprintf(line, sizeof(line), e.fmt, e.args[0], e.args[1], e.args[2], e.args[3]);
            fprintf(f, "[%08X] %s", e.thread, line);
            size_t n = strlen(line);
            if (n == 0 || line[n - 1] != '\n') fputc('\n', f);
        }
        fclose(f);
    }

    struct TraceInit {
        TraceInit() {
            std::set_terminate([] {
                dump_trace();
                vita_log("terminate: trace written\n");
                abort();
            });
        }
    } trace_init;
}

extern "C" void ultramodern_debug_printf(const char* fmt, ...) {
    // Queue listings ("  Contains:" then "%d (%d) " per thread) are dropped; a listing that never ends means a
    // cyclic queue, so write the trace out right away.
    static std::atomic<uint32_t> list_items{ 0 };
    if (fmt[0] == '%' && fmt[1] == 'd' && fmt[2] == ' ') {
        if (list_items.fetch_add(1) == 1000) {
            ultramodern_debug_printf("!!! cyclic thread queue detected\n");
            dump_trace();
            vita_log("cyclic thread queue: trace written\n");
            abort();
        }
        return;
    }
    if (fmt[0] == ' ' && fmt[1] == ' ' && fmt[2] == 'C') {
        list_items.store(0);
        return;
    }
    TraceEntry& e = trace_ring[trace_next.fetch_add(1) % trace_len];
    va_list ap;
    va_start(ap, fmt);
    for (int i = 0; i < 4; i++) e.args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    e.thread = 0;
    e.fmt = fmt;
}
#endif

#ifdef GAUNTLET_POISON_NEW
// Test aid: fill every operator-new allocation with garbage, as real hardware may hand it out (Vita3K zero-fills),
// so reads of uninitialised members show up in the emulator.
#include <new>
void* operator new(size_t n) { void* p = malloc(n ? n : 1); if (!p) abort(); memset(p, 0xA7, n); return p; }
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
#endif
