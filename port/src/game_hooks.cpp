// Game-specific runtime glue: boot loader replacement, TLB overlay mapping and hand-written asm helpers.
#include <cstdio>
#include <cstring>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "miniz.h"
#include "gauntlet_game.h"

namespace {
    inline uint32_t phys(int32_t vaddr) { return (uint32_t)vaddr & RDRAM_MASK; }

    // rdram holds each 32-bit word in host order, so big-endian ROM bytes are copied a word at a time.
    void copy_be_words(uint8_t* rdram, uint32_t dst_phys, const uint8_t* src, size_t size) {
        for (size_t i = 0; i < size; i += 4) {
            uint32_t w = (uint32_t(src[i]) << 24) | (uint32_t(src[i + 1]) << 16) | (uint32_t(src[i + 2]) << 8) | src[i + 3];
            *reinterpret_cast<uint32_t*>(rdram + dst_phys + i) = w;
        }
    }
}

// Replaces what the ROM's boot loader (ROM 0x1000, run at 0x80200000) does before jumping to the game:
// inflate the "game" segment, clear its bss and copy the segment table the overlay loader reads.
uint8_t* gauntlet_rdram = nullptr;
extern "C" void __sinf_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void __cosf_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void __sinf_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void __cosf_recomp(uint8_t* rdram, recomp_context* ctx);

void gauntlet::on_init(uint8_t* rdram, recomp_context* ctx) {
    gauntlet_rdram = rdram;
    std::span<const uint8_t> rom = recomp::get_rom();

    std::vector<uint8_t> game(0x100000);
    size_t out_size = tinfl_decompress_mem_to_mem(game.data(), game.size(), rom.data() + game_rom, game_rom_size, 0);
    if (out_size == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        fprintf(stderr, "[gauntlet] failed to inflate the game segment\n");
        std::abort();
    }
    copy_be_words(rdram, phys(game_vram), game.data(), (out_size + 3) & ~3);
    memset(rdram + phys(game_bss), 0, game_bss_size);
    copy_be_words(rdram, phys(game_segment_table_ram), rom.data() + rom_segment_table, segment_table_copy_size);

    gauntlet::load_all_sections();

    // The boot loader enters the game with its own stack (0x80200000 + 0x39A0); the entry function keeps
    // long-lived structs (e.g. a scheduler client) in that frame, so it must not be at address 0.
    ctx->r29 = (gpr)(int32_t)0x802039A0;
    fprintf(stderr, "[gauntlet] game segment inflated: 0x%zX bytes\n", out_size);

    if (getenv("GAUNTLET_TRIGTEST")) {
        recomp_context t = *ctx;
        const float xs[] = { 0.0f, 0.5f, 1.0f, 1.5708f, 3.0f, -2.0f };
        for (float x : xs) {
            t.f12.fl = x; __sinf_recomp(rdram, &t); float s = t.f0.fl;
            t.f12.fl = x; __cosf_recomp(rdram, &t); float c = t.f0.fl;
            fprintf(stderr, "[trig] x=%f sin=%f cos=%f\n", x, s, c);
        }
    }
}

extern "C" {

// osMapTLB(index, pagemask, vaddr, evenpaddr, oddpaddr, asid)
// The game maps overlays (0xE08xxxxx) onto heap blocks it has just inflated them into. The recomp addresses
// all memory as `addr & 0x1FFFFFFF`, so instead of aliasing, copy the block to the overlay's own address.
void osMapTLB_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t pagemask = (uint32_t)ctx->r5;
    uint32_t vaddr = (uint32_t)ctx->r6;
    int32_t even = (int32_t)ctx->r7;
    int32_t odd = MEM_W(0x10, ctx->r29);
    uint32_t page_size = (pagemask >> 1) + 0x1000;

    if (vaddr < 0xE0800000u || vaddr >= 0xE1000000u) {
        // Null-pointer guard pages and the boot-time identity map: nothing to do.
        return;
    }
    if (even != -1) {
        memmove(rdram + phys(vaddr), rdram + (even & RDRAM_MASK), page_size);
    }
    if (odd != -1) {
        memmove(rdram + phys(vaddr + page_size), rdram + (odd & RDRAM_MASK), page_size);
    }
}

void osUnmapTLB_recomp(uint8_t* rdram, recomp_context* ctx) {}
void osMapTLBRdb_recomp(uint8_t* rdram, recomp_context* ctx) {}

// Hand-written helpers at 0xE0080C54.. (ll/sc atomics and cache ops). Game threads never run concurrently.
void gl_atomic_or(uint8_t* rdram, recomp_context* ctx) {
    uint32_t old = MEM_W(0, ctx->r4);
    MEM_W(0, ctx->r4) = old | (uint32_t)ctx->r5;
    ctx->r2 = (gpr)(int32_t)(old & (uint32_t)ctx->r5);
}

void gl_atomic_andnot(uint8_t* rdram, recomp_context* ctx) {
    uint32_t old = MEM_W(0, ctx->r4);
    MEM_W(0, ctx->r4) = old & ~(uint32_t)ctx->r5;
    ctx->r2 = (gpr)(int32_t)(old & (uint32_t)ctx->r5);
}

void gl_atomic_inc(uint8_t* rdram, recomp_context* ctx) {
    int32_t old = MEM_W(0, ctx->r4);
    MEM_W(0, ctx->r4) = old + 1;
    ctx->r2 = (gpr)old;
}

void gl_atomic_dec(uint8_t* rdram, recomp_context* ctx) {
    int32_t old = MEM_W(0, ctx->r4);
    MEM_W(0, ctx->r4) = old - 1;
    ctx->r2 = (gpr)old;
}

void gl_cache_line(uint8_t* rdram, recomp_context* ctx) {}
void gl_cache_create_dirty(uint8_t* rdram, recomp_context* ctx) {}

void gl_get_sp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = ctx->r29;
}

void gl_interp_E0097620(uint8_t* rdram, recomp_context* ctx) { gauntlet::interp_run(rdram, ctx, 0xE0097620); }
void gl_interp_E0097714(uint8_t* rdram, recomp_context* ctx) { gauntlet::interp_run(rdram, ctx, 0xE0097714); }
void gl_interp_E0097994(uint8_t* rdram, recomp_context* ctx) { gauntlet::interp_run(rdram, ctx, 0xE0097994); }

}

// Low-level libultra internals still referenced by recompiled (but normally unreachable) pak/PI code.
// The runtime reimplements the osPfs*/osCont* entry points, so raw pak access reports "no pak".
extern "C" {
void __osContRamRead_recomp(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = 1; }   // PFS_ERR_NOPACK
void __osContRamWrite_recomp(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = 1; }
void __osPfsSelectBank_recomp(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = 1; }
void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = 0; }
void osPiWriteIo_recomp(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = 0; }
void osViGetCurrentLine_recomp(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = 0; }
void __osContDataCrc_recomp(uint8_t* rdram, recomp_context* ctx) { ctx->r2 = 0; }
}

extern "C" void yield_self_1ms(uint8_t* rdram);

// 0xE0003628 is an empty function the game calls inside its busy-wait loops. On the N64 interrupts would let
// other threads run meanwhile; here game threads only switch at OS calls, so yield to pending events instead.
extern "C" void gl_idle(uint8_t* rdram, recomp_context* ctx) {
    yield_self_1ms(rdram);
}

// Gauntlet packs its thread stacks tightly just below 4 MB (0xE03F3800..0xE0400000). Give each thread a
// roomy private stack in otherwise unused RAM instead, so a deep call chain can't run into a neighbour.
extern "C" int32_t gauntlet_thread_stack(int32_t sp, int32_t id) {
    uint32_t phys_sp = (uint32_t)sp & 0x1FFFFFFF;
    if (phys_sp < 0x3F0000 || phys_sp > 0x400000) {
        return sp;
    }
    int32_t new_sp = (int32_t)(0x80B00000u + (uint32_t)((id & 0xF) + 1) * 0x10000u);
    return new_sp;
}
