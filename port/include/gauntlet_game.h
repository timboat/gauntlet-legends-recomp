#ifndef __GAUNTLET_GAME_H__
#define __GAUNTLET_GAME_H__

#include <cstdint>
#include "recomp.h"

namespace gauntlet {
    // ROM layout (see tools/segs.py): boot loader segment table and the inflated "game" segment.
    constexpr uint32_t rom_segment_table = 0x12B0;   // copied by the boot loader to game_segment_table_ram
    constexpr uint32_t segment_table_copy_size = 0x1E0;
    constexpr int32_t game_segment_table_ram = (int32_t)0xE01301C0;
    constexpr uint32_t game_rom = 0x158D0;
    constexpr uint32_t game_rom_size = 0x4DE10;
    constexpr int32_t game_vram = (int32_t)0xE0000400;
    constexpr int32_t game_bss = (int32_t)0xE00ACEF0;
    constexpr uint32_t game_bss_size = 0x960A0;
    constexpr int32_t entrypoint = (int32_t)0xE0002850;

    // Hand-written GSM-style routines run by the interpreter.
    constexpr uint32_t interp_start = 0xE0097620;
    constexpr uint32_t interp_end = 0xE0097C20;
    inline bool interp_owns(uint32_t addr) { return addr >= interp_start && addr < interp_end; }
    void interp_run(uint8_t* rdram, recomp_context* ctx, uint32_t entry);

    void register_overlays();
    void load_all_sections();
    void on_init(uint8_t* rdram, recomp_context* ctx);

    namespace input {
        void init();
        void poll();
        bool get_input(int controller_num, uint16_t* buttons, float* x, float* y);
        void set_rumble(int controller_num, bool rumble);
        void handle_event(const void* sdl_event);
    }
}

#endif
