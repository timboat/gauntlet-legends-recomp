// Vita replacement for librecomp's recomp.cpp: the same game start-up path without mods, stored-ROM handling
// or a 4 GB address-space reservation. rdram is a plain 32 MB block (RDRAM_MASK = 0x01FFFFFF on this build).
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

#include "recomp.h"
#include "librecomp/overlays.hpp"
#include "librecomp/game.hpp"
#include "librecomp/addresses.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/error_handling.hpp"

static_assert(RDRAM_MASK == 0x01FFFFFF, "the Vita build expects a 32 MB rdram mask");
constexpr size_t vita_rdram_size = RDRAM_MASK + 1;

std::filesystem::path config_path;
static std::optional<recomp::GameEntry> game_entry;
static recomp::Version project_version;
static recomp::SaveType save_type = recomp::SaveType::None;
static std::mutex current_game_mutex;

enum GameStatus { None, Running, Quit };
static std::atomic<GameStatus> game_status = GameStatus::None;

std::u8string recomp::GameEntry::stored_filename() const {
    return game_id + u8".z64";
}

void recomp::register_config_path(std::filesystem::path path) {
    config_path = path;
}

bool recomp::register_game(const recomp::GameEntry& entry) {
    game_entry = entry;
    return true;
}

const recomp::Version& recomp::get_project_version() {
    return project_version;
}

extern "C" void osGetMemSize_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 8 * 1024 * 1024;
}

extern "C" void cop0_status_write(recomp_context* ctx, gpr value) {
    constexpr uint32_t FR = 0x04000000;
    uint32_t old_sr = ctx->status_reg;
    uint32_t new_sr = (uint32_t)value;
    uint32_t changed = old_sr ^ new_sr;
    if (changed & FR) {
        if (new_sr & FR) {
            ctx->f_odd = &ctx->f1.u32l;
            ctx->mips3_float_mode = true;
        } else {
            ctx->f_odd = &ctx->f0.u32h;
            ctx->mips3_float_mode = false;
        }
        changed &= ~FR;
    }
    if (changed) {
        printf("Unhandled status register bits changed: 0x%08X\n", changed);
        abort();
    }
    ctx->status_reg = new_sr;
}

extern "C" gpr cop0_status_read(recomp_context* ctx) {
    return (gpr)(int32_t)ctx->status_reg;
}

extern "C" void switch_error(const char* func, uint32_t vram, uint32_t jtbl) {
    printf("Switch-case out of bounds in %s at 0x%08X for jump table at 0x%08X\n", func, vram, jtbl);
    abort();
}

extern "C" void do_break(uint32_t vram) {
    printf("Encountered break at original vram 0x%08X\n", vram);
    abort();
}

void run_thread_function(uint8_t* rdram, uint64_t addr, uint64_t sp, uint64_t arg) {
    recomp_context ctx{};
    ctx.r29 = sp;
    ctx.r4 = arg;
    ctx.mips3_float_mode = 0;
    ctx.f_odd = &ctx.f0.u32h;
    if (game_entry->thread_create_callback != nullptr) {
        game_entry->thread_create_callback(rdram, &ctx);
    }
    recomp_func_t* func = get_function(addr);
    func(rdram, &ctx);
}

static void init(uint8_t* rdram, recomp_context* ctx, gpr entrypoint) {
    recomp::overlays::init_overlays();
    load_overlays(0x1000, (int32_t)entrypoint, 1024 * 1024);
    recomp::do_rom_read(rdram, entrypoint, 0x10001000, 0x100000);
    recomp::overlays::read_patch_data(rdram, (gpr)recomp::patch_rdram_start);

    ctx->f_odd = &ctx->f0.u32h;
    ctx->mips3_float_mode = false;

    // Variables normally set by IPL3.
    MEM_W(0x80000300, 0) = 1;             // osTvType: NTSC
    MEM_W(0x80000308, 0) = 0xB0000000u;   // osRomBase
    MEM_W(0x8000030C, 0) = 0;             // osResetType: cold
    MEM_W(0x80000318, 0) = 8 * 1024 * 1024; // osMemSize
}

std::u8string recomp::current_game_id() {
    std::lock_guard<std::mutex> lock(current_game_mutex);
    return game_entry->game_id;
}

std::string recomp::current_mod_game_id() {
    return game_entry->mod_game_id;
}

void recomp::start_game(const std::u8string& game_id) {
    game_status.store(GameStatus::Running);
    game_status.notify_all();
}

bool ultramodern::is_game_started() {
    return game_status.load() != GameStatus::None;
}

std::atomic_bool exited = false;
moodycamel::LightweightSemaphore graphics_shutdown_ready;

void ultramodern::quit() {
    exited.store(true);
    GameStatus desired = GameStatus::None;
    game_status.compare_exchange_strong(desired, GameStatus::Quit);
    game_status.notify_all();
}

recomp::SaveType recomp::get_save_type() { return save_type; }
bool recomp::eeprom_allowed() { return save_type == SaveType::Eep4k || save_type == SaveType::Eep16k || save_type == SaveType::AllowAll; }
bool recomp::sram_allowed() { return save_type == SaveType::Sram || save_type == SaveType::AllowAll; }
bool recomp::flashram_allowed() { return save_type == SaveType::Flashram || save_type == SaveType::AllowAll; }

static void run_game(uint8_t* rdram) {
    game_status.wait(GameStatus::None);
    if (game_status.load() != GameStatus::Running) {
        return;
    }
    recomp_context context{};
    init(rdram, &context, game_entry->entrypoint_address);
    if (game_entry->on_init_callback) {
        game_entry->on_init_callback(rdram, &context);
    }
    recomp::init_heap(rdram, recomp::mod_rdram_start);
    save_type = game_entry->save_type;
    ultramodern::init_saving(rdram);
    try {
        game_entry->entrypoint(rdram, &context);
    } catch (ultramodern::thread_terminated&) {
    }
}

void recomp::start(
    const recomp::Version& version,
    ultramodern::renderer::WindowHandle window_handle,
    const recomp::rsp::callbacks_t& rsp_callbacks,
    const ultramodern::renderer::callbacks_t& renderer_callbacks,
    const ultramodern::audio_callbacks_t& audio_callbacks,
    const ultramodern::input::callbacks_t& input_callbacks,
    const ultramodern::gfx_callbacks_t& gfx_callbacks_,
    const ultramodern::events::callbacks_t& events_callbacks,
    const ultramodern::error_handling::callbacks_t& error_handling_callbacks,
    const ultramodern::threads::callbacks_t& threads_callbacks
) {
    project_version = version;
    recomp::rsp::set_callbacks(rsp_callbacks);

    static const ultramodern::rsp::callbacks_t ultramodern_rsp_callbacks {
        .init = recomp::rsp::constants_init,
        .run_task = recomp::rsp::run_task,
    };
    ultramodern::set_callbacks(ultramodern_rsp_callbacks, renderer_callbacks, audio_callbacks, input_callbacks, gfx_callbacks_, events_callbacks, error_handling_callbacks, threads_callbacks);

    ultramodern::gfx_callbacks_t gfx_callbacks = gfx_callbacks_;
    ultramodern::gfx_callbacks_t::gfx_data_t gfx_data{};
    if (gfx_callbacks.create_gfx) {
        gfx_data = gfx_callbacks.create_gfx();
    }

    uint8_t* rdram = static_cast<uint8_t*>(aligned_alloc(0x10000, vita_rdram_size));
    if (rdram == nullptr) {
        ultramodern::error_handling::message_box("Failed to allocate memory!");
        return;
    }
    memset(rdram, 0, vita_rdram_size);
    recomp::register_heap_exports();

    std::thread game_thread{[](ultramodern::renderer::WindowHandle window_handle, uint8_t* rdram) {
        ultramodern::preinit(rdram, window_handle);
        run_game(rdram);
    }, window_handle, rdram};

    while (!exited) {
        ultramodern::sleep_milliseconds(1);
        if (gfx_callbacks.update_gfx != nullptr) {
            gfx_callbacks.update_gfx(gfx_data);
        }
    }

    graphics_shutdown_ready.signal();
    game_thread.join();
    ultramodern::join_event_threads();
    ultramodern::join_thread_cleaner_thread();
    ultramodern::join_saving_thread();
    ::free(rdram);
}

// The PC build's crash tracer (port/src/crash_trace.cpp) records message-queue activity; not used on the Vita.
extern "C" void gauntlet_trace_note(const char*, uint32_t, uint32_t) {}
