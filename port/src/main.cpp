// Gauntlet Legends: Recompiled - PC frontend (N64ModernRuntime + RT64, SDL2 window/audio/input).
#include <cstdio>
#include <cinttypes>
#include <filesystem>
#include <vector>
#include <array>
#include <stdexcept>
#include <chrono>

#define SDL_MAIN_HANDLED
#include "SDL.h"
#include "SDL_syswm.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <timeapi.h>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"

#include "gauntlet_game.h"
#include "gauntlet_render.h"

static const std::string version_string = "0.1.0";
static const std::u8string game_id = u8"gauntlet.n64.us";

static SDL_Window* window = nullptr;

[[noreturn]] static void exit_error(const char* msg) {
    fprintf(stderr, "%s\n", msg);
    MessageBoxA(nullptr, msg, "Gauntlet Legends: Recompiled", MB_OK | MB_ICONERROR);
    std::exit(EXIT_FAILURE);
}

static ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) > 0) {
        exit_error("Failed to initialize SDL2");
    }
    gauntlet::input::init();
    return {};
}

static ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    window = SDL_CreateWindow("Gauntlet Legends: Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 960, SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        exit_error("Failed to create window");
    }
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(window, &wmInfo);
    return ultramodern::renderer::WindowHandle{ wmInfo.info.win.window, GetCurrentThreadId() };
}

static void update_gfx(void*) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            ultramodern::quit();
        }
        gauntlet::input::handle_event(&event);
    }
}

// ---- Audio (same resampling scheme as Zelda64Recomp) ----
static SDL_AudioCVT audio_convert;
static SDL_AudioDeviceID audio_device = 0;
static uint32_t sample_rate = 48000;
static uint32_t output_sample_rate = 48000;
constexpr uint32_t input_channels = 2;
static uint32_t output_channels = 2;
constexpr uint32_t duplicated_input_frames = 4;
static uint32_t discarded_output_frames;
constexpr uint32_t bytes_per_frame = input_channels * sizeof(float);

// Debug: GAUNTLET_AUDIODUMP=<file> records the game's stereo output (16-bit, host order, L/R) to a raw file and logs
// once a second how many frames arrived, the current rate and whether the output queue had run dry (an audible gap).
static void debug_audio(const int16_t* audio_data, size_t sample_count) {
    static const char* path = getenv("GAUNTLET_AUDIODUMP");
    if (path == nullptr) return;
    static FILE* f = fopen(path, "wb");
    static auto last = std::chrono::steady_clock::now();
    static size_t frames = 0, calls = 0, empty = 0;
    if (f != nullptr) {
        for (size_t i = 0; i < sample_count; i += 2) {
            int16_t lr[2] = { audio_data[i + 1], audio_data[i] };
            fwrite(lr, sizeof(lr), 1, f);
        }
    }
    frames += sample_count / 2;
    calls++;
    empty += SDL_GetQueuedAudioSize(audio_device) == 0;
    auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(1)) {
        double secs = std::chrono::duration<double>(now - last).count();
        fprintf(stderr, "[audio] %.0f frames/s rate %u calls %zu queue-empty %zu queued %u bytes\n",
            frames / secs, sample_rate, calls, empty, SDL_GetQueuedAudioSize(audio_device));
        if (f != nullptr) fflush(f);
        frames = calls = empty = 0;
        last = now;
    }
}

static void queue_samples(int16_t* audio_data, size_t sample_count) {
    debug_audio(audio_data, sample_count);
    static std::vector<float> swap_buffer;
    static std::array<float, duplicated_input_frames * input_channels> duplicated_sample_buffer;

    size_t resampled_sample_count = sample_count + duplicated_input_frames * input_channels;
    size_t max_sample_count = std::max(resampled_sample_count, resampled_sample_count * audio_convert.len_mult);
    if (max_sample_count > swap_buffer.size()) {
        swap_buffer.resize(max_sample_count);
    }
    for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
        swap_buffer[i] = duplicated_sample_buffer[i];
    }
    // Swap channels to undo the 32-bit word swap of the sample buffer.
    for (size_t i = 0; i < sample_count; i += input_channels) {
        swap_buffer[i + 0 + duplicated_input_frames * input_channels] = audio_data[i + 1] * (0.5f / 32768.0f);
        swap_buffer[i + 1 + duplicated_input_frames * input_channels] = audio_data[i + 0] * (0.5f / 32768.0f);
    }
    if (sample_count <= duplicated_input_frames * input_channels) {
        return;
    }
    for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
        duplicated_sample_buffer[i] = swap_buffer[i + sample_count];
    }

    audio_convert.buf = reinterpret_cast<Uint8*>(swap_buffer.data());
    audio_convert.len = (sample_count + duplicated_input_frames * input_channels) * sizeof(swap_buffer[0]);
    if (SDL_ConvertAudio(&audio_convert) < 0) {
        fprintf(stderr, "SDL_ConvertAudio failed: %s\n", SDL_GetError());
        return;
    }

    uint64_t cur_queued_microseconds = uint64_t(SDL_GetQueuedAudioSize(audio_device)) / bytes_per_frame * 1000000 / sample_rate;
    uint32_t num_bytes_to_queue = audio_convert.len_cvt - output_channels * discarded_output_frames * sizeof(swap_buffer[0]);
    float* samples_to_queue = swap_buffer.data() + output_channels * discarded_output_frames / 2;

    uint32_t skip_factor = cur_queued_microseconds / 100000;
    if (skip_factor != 0) {
        uint32_t skip_ratio = 1 << skip_factor;
        num_bytes_to_queue /= skip_ratio;
        for (size_t i = 0; i < num_bytes_to_queue / (output_channels * sizeof(swap_buffer[0])); i++) {
            samples_to_queue[2 * i + 0] = samples_to_queue[2 * skip_ratio * i + 0];
            samples_to_queue[2 * i + 1] = samples_to_queue[2 * skip_ratio * i + 1];
        }
    }
    SDL_QueueAudio(audio_device, samples_to_queue, num_bytes_to_queue);
}

static size_t get_frames_remaining() {
    constexpr float buffer_offset_frames = 1.0f;
    uint64_t buffered_byte_count = SDL_GetQueuedAudioSize(audio_device);
    buffered_byte_count = buffered_byte_count * 2 * sample_rate / output_sample_rate / output_channels;
    uint32_t frames_per_vi = (sample_rate / 60);
    if (buffered_byte_count > (buffer_offset_frames * bytes_per_frame * frames_per_vi)) {
        buffered_byte_count -= (buffer_offset_frames * bytes_per_frame * frames_per_vi);
    }
    else {
        buffered_byte_count = 0;
    }
    return static_cast<uint32_t>(buffered_byte_count / bytes_per_frame);
}

static void update_audio_converter() {
    if (SDL_BuildAudioCVT(&audio_convert, AUDIO_F32, input_channels, sample_rate, AUDIO_F32, output_channels, output_sample_rate) < 0) {
        exit_error("Error creating SDL audio converter");
    }
    discarded_output_frames = duplicated_input_frames * output_sample_rate / sample_rate;
}

static void set_frequency(uint32_t freq) {
    sample_rate = freq;
    update_audio_converter();
}

static void reset_audio(uint32_t output_freq) {
    SDL_AudioSpec spec_desired{};
    spec_desired.freq = (int)output_freq;
    spec_desired.format = AUDIO_F32;
    spec_desired.channels = (Uint8)output_channels;
    spec_desired.samples = 0x100;
    audio_device = SDL_OpenAudioDevice(nullptr, false, &spec_desired, nullptr, 0);
    if (audio_device == 0) {
        exit_error("SDL error opening audio device");
    }
    SDL_PauseAudioDevice(audio_device, 0);
    output_sample_rate = output_freq;
    update_audio_converter();
}

// ---- RSP ----
RspExitReason gauntlet_audio_ucode(uint8_t* rdram, uint32_t ucode_addr);

// Debug: GAUNTLET_NO_AUDIO skips audio tasks entirely.
static RspExitReason skip_audio_ucode(uint8_t* rdram, uint32_t ucode_addr) { return RspExitReason::Broke; }

static RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    static const bool no_audio = getenv("GAUNTLET_NO_AUDIO") != nullptr;
    switch (task->t.type) {
        case M_AUDTASK:
            return no_audio ? skip_audio_ucode : gauntlet_audio_ucode;
        default:
            fprintf(stderr, "Unknown RSP task type: %" PRIu32 "\n", task->t.type);
            return nullptr;
    }
}

// ---- Game entry ----
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    // Port 1 always has a controller (keyboard); ports 2-4 only when a game pad is plugged in for them.
    // Each controller holds a Controller Pak (data/saves/controller_pak_N.mpk), where the game keeps its saves.
    uint16_t buttons; float x, y;
    bool present = controller_num == 0 || gauntlet::input::get_input(controller_num, &buttons, &x, &y);
    return { present ? ultramodern::input::Device::Controller : ultramodern::input::Device::None, ultramodern::input::Pak::ControllerPak };
}

static void message_box(const char* msg) {
    fprintf(stderr, "%s\n", msg);
    MessageBoxA(nullptr, msg, "Gauntlet Legends: Recompiled", MB_OK);
}

static std::filesystem::path program_dir() {
    wchar_t module_name[MAX_PATH];
    GetModuleFileNameW(NULL, module_name, MAX_PATH);
    return std::filesystem::path(module_name).parent_path();
}

int main(int argc, char** argv) {
    recomp::Version project_version{};
    recomp::Version::from_string(version_string, project_version);

    timeBeginPeriod(1);
    SetConsoleOutputCP(CP_UTF8);
    SDL_setenv("SDL_AUDIODRIVER", "wasapi", true);

    SDL_InitSubSystem(SDL_INIT_AUDIO);
    reset_audio(48000);

    std::filesystem::path data_dir = program_dir() / "data";
    std::filesystem::create_directories(data_dir);
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

    // Find the ROM: stored copy, command line, or next to the executable / in ../rom.
    recomp::check_all_stored_roms();
    std::u8string id = game_id;
    if (!recomp::is_rom_valid(id)) {
        std::vector<std::filesystem::path> candidates;
        if (argc > 1) candidates.emplace_back(argv[1]);
        candidates.emplace_back(program_dir() / "Gauntlet Legends (USA).z64");
        candidates.emplace_back(program_dir() / ".." / ".." / "rom" / "Gauntlet Legends (USA).z64");
        bool ok = false;
        for (const auto& path : candidates) {
            if (std::filesystem::exists(path) && recomp::select_rom(path, id) == recomp::RomValidationError::Good) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            exit_error("Couldn't find a valid Gauntlet Legends (USA) ROM.\nPass the .z64 path on the command line or put it next to GauntletRecomp.exe.");
        }
    }

    recomp::rsp::callbacks_t rsp_callbacks{ .get_rsp_microcode = get_rsp_microcode };
    ultramodern::renderer::callbacks_t renderer_callbacks{ .create_render_context = gauntlet::renderer::create_render_context };
    ultramodern::gfx_callbacks_t gfx_callbacks{ .create_gfx = create_gfx, .create_window = create_window, .update_gfx = update_gfx };
    ultramodern::audio_callbacks_t audio_callbacks{ .queue_samples = queue_samples, .get_frames_remaining = get_frames_remaining, .set_frequency = set_frequency };
    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = gauntlet::input::poll,
        .get_input = gauntlet::input::get_input,
        .set_rumble = gauntlet::input::set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };
    ultramodern::events::callbacks_t events_callbacks{};
    ultramodern::error_handling::callbacks_t error_handling_callbacks{ .message_box = message_box };
    ultramodern::threads::callbacks_t threads_callbacks{};

    recomp::start_game(game_id);
    recomp::start(project_version, {}, rsp_callbacks, renderer_callbacks, audio_callbacks, input_callbacks,
        gfx_callbacks, events_callbacks, error_handling_callbacks, threads_callbacks);

    timeEndPeriod(1);
    return EXIT_SUCCESS;
}
