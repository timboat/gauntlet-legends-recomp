#include <memory>
#include <thread>
#include <chrono>
#include <cstring>
#include <variant>
#include <vector>
#include <algorithm>

#define HLSL_CPU
#include "hle/rt64_application.h"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"

#include "gauntlet_render.h"
#include "n64gfx.h"
#include <chrono>

// Comparison mode for the Vita renderer: GAUNTLET_SOFTGX=1 also runs gfx/n64gfx (software backend) on every
// display list; creating softgx.go next to the exe writes the frame it presents next to softgx_N.ppm.
namespace {
    struct SoftGx {
        n64gfx::SoftBackend gpu;
        n64gfx::Renderer renderer;
        int shots = 0;
        SoftGx(uint8_t* rdram, bool to_rdram) : gpu(rdram, false), renderer(rdram, &gpu, to_rdram) {}
    };
    std::unique_ptr<SoftGx> softgx;

    // GAUNTLET_SOFTGX=3: n64gfx with a GPU backend that discards draws, timed per display list (CPU cost of the
    // Vita renderer without its GPU), reported to stderr every 300 lists.
    struct NullBackend final : n64gfx::Backend {
        void begin_target(uint32_t, int, int) override {}
        void draw(const n64gfx::PassState&, const n64gfx::Vertex*, int) override {}
        void forget_texture(uint32_t) override {}
    };
    struct CpuProfile {
        NullBackend gpu;
        n64gfx::Renderer renderer;
        double secs = 0;
        int lists = 0;
        CpuProfile(uint8_t* rdram) : renderer(rdram, &gpu, false) {}
        void run(uint32_t dl) {
            auto t0 = std::chrono::steady_clock::now();
            renderer.run(dl);
            secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (++lists == 300) {
                auto& s = renderer.stats;
                static n64gfx::Renderer::Stats last{};
                fprintf(stderr, "[cpuprof] %.3f ms/list  tris %u draws %u passes %u per list\n",
                    secs * 1000 / lists, (s.tris - last.tris) / lists, (s.draws - last.draws) / lists, (s.passes - last.passes) / lists);
                for (int p = 0; p < 6; p++)
                    fprintf(stderr, "[cpuprof]   path %d: %u draws %u passes per list\n", p, (s.path_draws[p] - last.path_draws[p]) / lists,
                        (s.path_passes[p] - last.path_passes[p]) / lists);
                fprintf(stderr, "[cpuprof]   split per triangle: %u draws per list\n", (s.split_draws - last.split_draws) / lists);
                fprintf(stderr, "[cpuprof]   bad cmds total %u\n", s.bad_cmds);
                last = s;
                secs = 0;
                lists = 0;
            }
        }
    };
    std::unique_ptr<CpuProfile> cpuprof;

    void softgx_write(const std::vector<uint32_t>& px, int w, int h, const char* path) {
        FILE* f = fopen(path, "wb");
        if (!f) return;
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (uint32_t c : px) { uint8_t rgb[3] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16) }; fwrite(rgb, 1, 3, f); }
        fclose(f);
    }
}

static RT64::UserConfiguration::Antialiasing device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
static bool sample_positions_supported = false;
static bool high_precision_fb_enabled = false;

static uint8_t DMEM[0x1000];
static uint8_t IMEM[0x1000];

unsigned int MI_INTR_REG = 0;

unsigned int DPC_START_REG = 0;
unsigned int DPC_END_REG = 0;
unsigned int DPC_CURRENT_REG = 0;
unsigned int DPC_STATUS_REG = 0;
unsigned int DPC_CLOCK_REG = 0;
unsigned int DPC_BUFBUSY_REG = 0;
unsigned int DPC_PIPEBUSY_REG = 0;
unsigned int DPC_TMEM_REG = 0;

void dummy_check_interrupts() {}

RT64::UserConfiguration::Antialiasing compute_max_supported_aa(RT64::RenderSampleCounts bits) {
    if (bits & RT64::RenderSampleCount::Bits::COUNT_2) {
        if (bits & RT64::RenderSampleCount::Bits::COUNT_4) {
            if (bits & RT64::RenderSampleCount::Bits::COUNT_8) {
                return RT64::UserConfiguration::Antialiasing::MSAA8X;
            }
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        }
        return RT64::UserConfiguration::Antialiasing::MSAA2X;
    };
    return RT64::UserConfiguration::Antialiasing::None;
}

RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
    switch (option) {
        case ultramodern::renderer::AspectRatio::Original:
            return RT64::UserConfiguration::AspectRatio::Original;
        case ultramodern::renderer::AspectRatio::Expand:
            return RT64::UserConfiguration::AspectRatio::Expand;
        case ultramodern::renderer::AspectRatio::Manual:
            return RT64::UserConfiguration::AspectRatio::Manual;
        case ultramodern::renderer::AspectRatio::OptionCount:
            return RT64::UserConfiguration::AspectRatio::OptionCount;
    }
}

RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing option) {
    switch (option) {
        case ultramodern::renderer::Antialiasing::None:
            return RT64::UserConfiguration::Antialiasing::None;
        case ultramodern::renderer::Antialiasing::MSAA2X:
            return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case ultramodern::renderer::Antialiasing::MSAA4X:
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case ultramodern::renderer::Antialiasing::MSAA8X:
            return RT64::UserConfiguration::Antialiasing::MSAA8X;
        case ultramodern::renderer::Antialiasing::OptionCount:
            return RT64::UserConfiguration::Antialiasing::OptionCount;
    }
}

RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate option) {
    switch (option) {
        case ultramodern::renderer::RefreshRate::Original:
            return RT64::UserConfiguration::RefreshRate::Original;
        case ultramodern::renderer::RefreshRate::Display:
            return RT64::UserConfiguration::RefreshRate::Display;
        case ultramodern::renderer::RefreshRate::Manual:
            return RT64::UserConfiguration::RefreshRate::Manual;
        case ultramodern::renderer::RefreshRate::OptionCount:
            return RT64::UserConfiguration::RefreshRate::OptionCount;
    }
}

RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer option) {
    switch (option) {
        case ultramodern::renderer::HighPrecisionFramebuffer::Off:
            return RT64::UserConfiguration::InternalColorFormat::Standard;
        case ultramodern::renderer::HighPrecisionFramebuffer::On:
            return RT64::UserConfiguration::InternalColorFormat::High;
        case ultramodern::renderer::HighPrecisionFramebuffer::Auto:
            return RT64::UserConfiguration::InternalColorFormat::Automatic;
        case ultramodern::renderer::HighPrecisionFramebuffer::OptionCount:
            return RT64::UserConfiguration::InternalColorFormat::OptionCount;
    }
}

void set_application_user_config(RT64::Application* application, const ultramodern::renderer::GraphicsConfig& config) {
    switch (config.res_option) {
        default:
        case ultramodern::renderer::Resolution::Auto:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            application->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
        case ultramodern::renderer::Resolution::Original2x:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = 2.0 * std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
    }

    switch (config.hr_option) {
        default:
        case ultramodern::renderer::HUDRatioMode::Original:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
            break;
        case ultramodern::renderer::HUDRatioMode::Clamp16x9:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
            application->userConfig.extAspectTarget = 16.0/9.0;
            break;
        case ultramodern::renderer::HUDRatioMode::Full:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
            break;
    }

    application->userConfig.aspectRatio = to_rt64(config.ar_option);
    application->userConfig.antialiasing = to_rt64(config.msaa_option);
    application->userConfig.refreshRate = to_rt64(config.rr_option);
    application->userConfig.refreshRateTarget = config.rr_manual_value;
    application->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
}

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult rt64_result) {
    switch (rt64_result) {
        case RT64::Application::SetupResult::Success:
            return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound:
            return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:
            return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:
            return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:
            return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }

    fprintf(stderr, "Unhandled `RT64::Application::SetupResult` ?\n");
    assert(false);
    std::exit(EXIT_FAILURE);
}

ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12:
            return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan:
            return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal:
            return ultramodern::renderer::GraphicsApi::Metal;
        case RT64::UserConfiguration::GraphicsAPI::Automatic:
            return ultramodern::renderer::GraphicsApi::Auto;
    }

    fprintf(stderr, "Unhandled `RT64::UserConfiguration::GraphicsAPI` ?\n");
    assert(false);
    std::exit(EXIT_FAILURE);
}

gauntlet::renderer::RT64Context::RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool debug) {
    static unsigned char dummy_rom_header[0x40];

    // Set up the RT64 application core fields.
    RT64::Application::Core appCore{};
#if defined(_WIN32)
    appCore.window = window_handle.window;
#elif defined(__linux__) || defined(__ANDROID__)
    appCore.window = window_handle;
#elif defined(__APPLE__)
    appCore.window.window = window_handle.window;
    appCore.window.view = window_handle.view;
#endif

    appCore.checkInterrupts = dummy_check_interrupts;

    appCore.HEADER = dummy_rom_header;
    appCore.RDRAM = rdram;
    appCore.DMEM = DMEM;
    appCore.IMEM = IMEM;

    appCore.MI_INTR_REG = &MI_INTR_REG;

    appCore.DPC_START_REG = &DPC_START_REG;
    appCore.DPC_END_REG = &DPC_END_REG;
    appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
    appCore.DPC_STATUS_REG = &DPC_STATUS_REG;
    appCore.DPC_CLOCK_REG = &DPC_CLOCK_REG;
    appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
    appCore.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
    appCore.DPC_TMEM_REG = &DPC_TMEM_REG;

    ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();

    appCore.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
    appCore.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
    appCore.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
    appCore.VI_INTR_REG = &vi_regs->VI_INTR_REG;
    appCore.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
    appCore.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
    appCore.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
    appCore.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
    appCore.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
    appCore.VI_H_START_REG = &vi_regs->VI_H_START_REG;
    appCore.VI_V_START_REG = &vi_regs->VI_V_START_REG;
    appCore.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
    appCore.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
    appCore.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

    // Set up the RT64 application configuration fields.
    RT64::ApplicationConfiguration appConfig;
    appConfig.useConfigurationFile = false;

    // Create the RT64 application.
    app = std::make_unique<RT64::Application>(appCore, appConfig);

    // Set initial user config settings based on the current settings.
    auto& cur_config = ultramodern::renderer::get_graphics_config();
    set_application_user_config(app.get(), cur_config);
    app->userConfig.developerMode = debug;
    // Pick an API if the user has set an override.
    switch (cur_config.api_option) {
        case ultramodern::renderer::GraphicsApi::D3D12:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
            break;
        case ultramodern::renderer::GraphicsApi::Vulkan:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
            break;
        case ultramodern::renderer::GraphicsApi::Metal:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal;
            break;
        case ultramodern::renderer::GraphicsApi::Auto:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
            break;
    }

    // Set up the RT64 application.
    uint32_t thread_id = 0;
#ifdef _WIN32
    thread_id = window_handle.thread_id;
#endif
    setup_result = map_setup_result(app->setup(thread_id));
    // Get the API that RT64 chose.
    chosen_api = map_graphics_api(app->chosenGraphicsAPI);
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        app = nullptr;
        return;
    }

    // Set the application's fullscreen state.
    app->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);

    // Check if the selected device actually supports MSAA sample positions and MSAA for for the formats that will be used
    // and downgrade the configuration accordingly.
    if (app->device->getCapabilities().sampleLocations) {
        RT64::RenderSampleCounts color_sample_counts = app->device->getSampleCountsSupported(RT64::RenderFormat::R8G8B8A8_UNORM);
        RT64::RenderSampleCounts depth_sample_counts = app->device->getSampleCountsSupported(RT64::RenderFormat::D32_FLOAT);
        RT64::RenderSampleCounts common_sample_counts = color_sample_counts & depth_sample_counts;
        device_max_msaa = compute_max_supported_aa(common_sample_counts);
        sample_positions_supported = true;
    }
    else {
        device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
        sample_positions_supported = false;
    }

    high_precision_fb_enabled = app->shaderLibrary->usesHDR;
}

gauntlet::renderer::RT64Context::~RT64Context() = default;

#include <chrono>
extern "C" void gauntlet_print_mtx_records(FILE* f);
extern "C" void gauntlet_print_mul_records(FILE* f);

// Debug: GAUNTLET_DLDUMP=<seconds> dumps 8 graphics tasks, 0.5 s apart, from that time after the first task (F3DEX2 commands, matrices, vertices).
static void dump_display_list(uint8_t* rdram, uint32_t dl, const char* path) {
    uint32_t segments[16] = {};
    uint32_t stack[32]; int sp = 0;
    auto rd = [&](uint32_t a) { return *(uint32_t*)(rdram + (a & 0xFFFFFF)); };
    auto seg = [&](uint32_t a) { return segments[(a >> 24) & 0xF] + (a & 0xFFFFFF); };
    FILE* f = fopen(path, "w");
    if (f == nullptr) return;
    for (int n = 0; n < 100000; n++) {
        uint32_t w0 = rd(dl), w1 = rd(dl + 4);
        fprintf(f, "%06X%*s %08X %08X\n", dl & 0xFFFFFF, sp * 2, "", w0, w1);
        dl += 8;
        uint8_t op = w0 >> 24;
        if (op == 0xDF) { if (sp == 0) break; dl = stack[--sp]; }
        else if (op == 0xDE) { if (((w0 >> 16) & 0xFF) == 0 && sp < 32) stack[sp++] = dl; dl = seg(w1); }
        else if (op == 0xDB && ((w0 >> 16) & 0xFF) == 6) segments[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0xFFFFFF;
        else if (op == 0xDA) {
            uint32_t m = seg(w1);
            fprintf(f, "        mtx @%06X params %02X:", m & 0xFFFFFF, (w0 & 0xFF) ^ 1);
            for (int i = 0; i < 16; i++) {
                uint32_t iw = rd(m + (i / 2) * 4), fw = rd(m + 32 + (i / 2) * 4);
                int16_t ip = (int16_t)((i & 1) ? (iw & 0xFFFF) : (iw >> 16));
                uint16_t fp = (uint16_t)((i & 1) ? (fw & 0xFFFF) : (fw >> 16));
                fprintf(f, "%s%.4f", (i % 4) ? " " : " | ", ip + fp / 65536.0);
            }
            fprintf(f, "\n");
        }
        else if (op == 0x01) {
            uint32_t v = seg(w1), cnt = (w0 >> 12) & 0xFF;
            fprintf(f, "        vtx @%06X n %u end %u:", v & 0xFFFFFF, cnt, (w0 & 0xFF) >> 1);
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t a0 = rd(v + i * 16), a1 = rd(v + i * 16 + 4);
                fprintf(f, " (%d %d %d)", (int16_t)(a0 >> 16), (int16_t)a0, (int16_t)(a1 >> 16));
            }
            fprintf(f, "\n");
        }
    }
    gauntlet_print_mtx_records(f);
    gauntlet_print_mul_records(f);
    fclose(f);
    fprintf(stderr, "[dl] dumped to dldump.txt\n");
}

void gauntlet::renderer::RT64Context::send_dl(const OSTask* task) {
    app->state->rsp->reset();
    app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
    static const auto start = std::chrono::steady_clock::now();
    static const char* dump_at = getenv("GAUNTLET_DLDUMP");
    static int dumps = 0;
    if (dump_at != nullptr && dumps < 8 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= atof(dump_at) + dumps * 0.5) {
        char path[32];
        snprintf(path, sizeof(path), "dldump%d.txt", dumps++);
        dump_display_list(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, path);
    }
    // Debug: GAUNTLET_HUDWATCH logs which rows/columns of the 416x50 HUD buffer (0x45F3E0) change in RDRAM between tasks.
    static const bool hud_watch = getenv("GAUNTLET_HUDWATCH") != nullptr;
    if (hud_watch) {
        constexpr uint32_t base = 0x45F3E0, w = 416, h = 50;
        static std::vector<uint16_t> prev(w * h);
        static int ntask = 0;
        const uint16_t* cur = (const uint16_t*)(app->core.RDRAM + base);
        for (uint32_t y = 0; y < h; y++) {
            int x0 = -1, x1 = -1;
            for (uint32_t x = 0; x < w; x++) {
                // RDRAM is word-swapped on PC: halfword at byte offset o lives at o ^ 2.
                if (cur[(y * w + x) ^ 1] != prev[y * w + x]) { if (x0 < 0) x0 = x; x1 = x; prev[y * w + x] = cur[(y * w + x) ^ 1]; }
            }
            if (x0 >= 0) fprintf(stderr, "[hud] task %d row %u x %d-%d\n", ntask, y, x0, x1);
        }
        ntask++;
    }
    // Debug: creating dldump.go next to the exe dumps the next 3 graphics tasks to dlgo0-2.txt.
    static int task_count = 0, go_left = 0;
    if (go_left == 0 && (++task_count % 30) == 0 && remove("dldump.go") == 0) go_left = 3;
    if (go_left > 0) {
        char path[32];
        snprintf(path, sizeof(path), "dlgo%d.txt", 3 - go_left--);
        dump_display_list(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, path);
    }
    // GAUNTLET_SOFTGX=2: as on the Vita, n64gfx draws offscreen buffers (the HUD) into RDRAM, before RT64 and
    // with RT64's own RDRAM write-back off.
    static const int softgx_mode = getenv("GAUNTLET_SOFTGX") ? atoi(getenv("GAUNTLET_SOFTGX")) : 0;
    // GAUNTLET_RENDER_DELAY=ms: wait before reading the list, as the Vita's slower renderer does (exposes races
    // between the game and a list still being read).
    static const int render_delay = getenv("GAUNTLET_RENDER_DELAY") ? atoi(getenv("GAUNTLET_RENDER_DELAY")) : 0;
    if (render_delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(render_delay));
    if (softgx_mode == 3) {
        if (!cpuprof) cpuprof = std::make_unique<CpuProfile>(app->core.RDRAM);
        cpuprof->run(task->t.data_ptr & 0x3FFFFFF);
    }
    if (softgx_mode && softgx_mode != 3 && !softgx) {
        softgx = std::make_unique<SoftGx>(app->core.RDRAM, softgx_mode == 2);
        if (softgx_mode == 2) app->state->setRenderToRAM(0);
    }
    if (softgx_mode == 2) softgx->renderer.run(task->t.data_ptr & 0x3FFFFFF);
    app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
    if (softgx_mode == 1) softgx->renderer.run(task->t.data_ptr & 0x3FFFFFF);
}

void gauntlet::renderer::RT64Context::update_screen() {
    app->updateScreen();
    if (cpuprof) cpuprof->renderer.note_vi_origin(ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG);
    if (softgx) {
        uint32_t origin = ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG;
        softgx->renderer.note_vi_origin(origin);
        if (remove("softgx.go") == 0) {
            char path[64];
            uint32_t addr = softgx->renderer.gpu_target_for(origin);
            int w = 0, h = 0;
            const std::vector<uint32_t>* img = addr ? softgx->gpu.image(addr, &w, &h) : nullptr;
            snprintf(path, sizeof(path), "softgx_%d.ppm", softgx->shots);
            if (img) softgx_write(*img, w, h, path);
            // The HUD buffer, drawn offscreen.
            if ((img = softgx->renderer.offscreen()->image(0x45F3E0, &w, &h))) {
                snprintf(path, sizeof(path), "softgx_%d_hud.ppm", softgx->shots);
                softgx_write(*img, w, h, path);
            } else {
                std::vector<uint32_t> hud;
                softgx->renderer.decode_rdram(0x45F3E0, 416, 50, hud);
                snprintf(path, sizeof(path), "softgx_%d_hud.ppm", softgx->shots);
                softgx_write(hud, 416, 50, path);
            }
            const auto& st = softgx->renderer.stats;
            fprintf(stderr, "[softgx] shot %d origin %08X target %08X tris %u draws %u passes %u decodes %u textures %u\n",
                softgx->shots, origin, addr, st.tris, st.draws, st.passes, st.tex_decodes, st.textures);
            softgx->shots++;
        }
    }
}

void gauntlet::renderer::RT64Context::shutdown() {
    if (app != nullptr) {
        app->end();
    }
}

bool gauntlet::renderer::RT64Context::update_config(const ultramodern::renderer::GraphicsConfig& old_config, const ultramodern::renderer::GraphicsConfig& new_config) {
    if (old_config == new_config) {
        return false;
    }

    if (new_config.wm_option != old_config.wm_option) {
        app->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    }

    set_application_user_config(app.get(), new_config);

    app->updateUserConfig(true);

    if (new_config.msaa_option != old_config.msaa_option) {
        app->updateMultisampling();
    }
    return true;
}

void gauntlet::renderer::RT64Context::enable_instant_present() {
    // Enable the present early presentation mode for minimal latency.
    app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;

    app->updateEnhancementConfig();
}

uint32_t gauntlet::renderer::RT64Context::get_display_framerate() const {
    return app->presentQueue->ext.sharedResources->swapChainRate;
}

float gauntlet::renderer::RT64Context::get_resolution_scale() const {
    constexpr int ReferenceHeight = 240;
    switch (app->userConfig.resolution) {
        case RT64::UserConfiguration::Resolution::WindowIntegerScale:
            if (app->sharedQueueResources->swapChainHeight > 0) {
                return std::max(float((app->sharedQueueResources->swapChainHeight + ReferenceHeight - 1) / ReferenceHeight), 1.0f);
            }
            else {
                return 1.0f;
            }
        case RT64::UserConfiguration::Resolution::Manual:
            return float(app->userConfig.resolutionMultiplier);
        case RT64::UserConfiguration::Resolution::Original:
        default:
            return 1.0f;
    }
}

std::unique_ptr<ultramodern::renderer::RendererContext> gauntlet::renderer::create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<gauntlet::renderer::RT64Context>(rdram, window_handle, developer_mode);
}
