// Minimal MIPS III integer interpreter for the few hand-written asm routines N64Recomp can't translate
// (PC capture via `bltzal $zero`, indirect jumps into the middle of a routine).
// Runs on the recomp register context and rdram, so it works the same on PC and Vita.
#include <cstdio>
#include <cstdlib>
#include "recomp.h"
#include "librecomp/overlays.hpp"
#include "gauntlet_game.h"

extern "C" recomp_func_t* get_function(int32_t addr);

namespace {
    constexpr uint32_t return_sentinel = 0xDEAD0000u;

    inline gpr& reg(recomp_context* ctx, uint32_t r) { return (&ctx->r0)[r]; }
    inline gpr sx32(uint32_t v) { return (gpr)(int64_t)(int32_t)v; }

    uint32_t fetch(uint8_t* rdram, uint32_t pc) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)pc);
    }

    [[noreturn]] void unhandled(uint32_t pc, uint32_t insn) {
        fprintf(stderr, "[interp] unhandled instruction %08X at %08X\n", insn, pc);
        std::abort();
    }
}

// Interpret starting at `entry` until the routine returns to its caller.
void gauntlet::interp_run(uint8_t* rdram, recomp_context* ctx, uint32_t entry) {
    gpr saved_ra = ctx->r31;
    ctx->r31 = sx32(return_sentinel);
    uint32_t pc = entry;
    // Branch handling: when `pending` is set, `pending_target` is taken after the next (delay slot) instruction.
    bool pending = false;
    uint32_t pending_target = 0;

    while (true) {
        uint32_t insn = fetch(rdram, pc);
        uint32_t op = insn >> 26, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31, rd = (insn >> 11) & 31;
        uint32_t sa = (insn >> 6) & 31, funct = insn & 63;
        int32_t simm = (int16_t)(insn & 0xFFFF);
        uint32_t uimm = insn & 0xFFFF;
        uint32_t next_pc = pc + 4;
        bool was_pending = pending;
        uint32_t was_target = pending_target;
        pending = false;
        auto R = [&](uint32_t r) -> gpr { return r ? reg(ctx, r) : 0; };
        auto W = [&](uint32_t r, gpr v) { if (r) reg(ctx, r) = v; };
        auto branch = [&](bool cond, bool likely, bool link) {
            if (link) W(31, sx32(pc + 8));
            if (cond) { pending = true; pending_target = pc + 4 + (simm << 2); }
            else if (likely) next_pc = pc + 8; // skip delay slot
        };

        switch (op) {
            case 0x00: // SPECIAL
                switch (funct) {
                    case 0x00: W(rd, sx32((uint32_t)R(rt) << sa)); break;
                    case 0x02: W(rd, sx32((uint32_t)R(rt) >> sa)); break;
                    case 0x03: W(rd, sx32((int32_t)R(rt) >> sa)); break;
                    case 0x04: W(rd, sx32((uint32_t)R(rt) << (R(rs) & 31))); break;
                    case 0x06: W(rd, sx32((uint32_t)R(rt) >> (R(rs) & 31))); break;
                    case 0x07: W(rd, sx32((int32_t)R(rt) >> (R(rs) & 31))); break;
                    case 0x08: pending = true; pending_target = (uint32_t)R(rs); break;
                    case 0x09: { uint32_t t = (uint32_t)R(rs); W(rd, sx32(pc + 8)); pending = true; pending_target = t; break; }
                    case 0x10: W(rd, ctx->hi); break;
                    case 0x11: ctx->hi = R(rs); break;
                    case 0x12: W(rd, ctx->lo); break;
                    case 0x13: ctx->lo = R(rs); break;
                    case 0x18: { int64_t p = (int64_t)(int32_t)R(rs) * (int32_t)R(rt); ctx->lo = sx32((uint32_t)p); ctx->hi = sx32((uint32_t)(p >> 32)); break; }
                    case 0x19: { uint64_t p = (uint64_t)(uint32_t)R(rs) * (uint32_t)R(rt); ctx->lo = sx32((uint32_t)p); ctx->hi = sx32((uint32_t)(p >> 32)); break; }
                    case 0x1A: { int32_t a = (int32_t)R(rs), b = (int32_t)R(rt); if (b) { ctx->lo = sx32((uint32_t)(a / b)); ctx->hi = sx32((uint32_t)(a % b)); } break; }
                    case 0x1B: { uint32_t a = (uint32_t)R(rs), b = (uint32_t)R(rt); if (b) { ctx->lo = sx32(a / b); ctx->hi = sx32(a % b); } break; }
                    case 0x20: case 0x21: W(rd, sx32((uint32_t)R(rs) + (uint32_t)R(rt))); break;
                    case 0x22: case 0x23: W(rd, sx32((uint32_t)R(rs) - (uint32_t)R(rt))); break;
                    case 0x24: W(rd, R(rs) & R(rt)); break;
                    case 0x25: W(rd, R(rs) | R(rt)); break;
                    case 0x26: W(rd, R(rs) ^ R(rt)); break;
                    case 0x27: W(rd, ~(R(rs) | R(rt))); break;
                    case 0x2A: W(rd, (int64_t)R(rs) < (int64_t)R(rt)); break;
                    case 0x2B: W(rd, R(rs) < R(rt)); break;
                    default: unhandled(pc, insn);
                }
                break;
            case 0x01: // REGIMM
                switch (rt) {
                    case 0x00: branch((int64_t)R(rs) < 0, false, false); break;
                    case 0x01: branch((int64_t)R(rs) >= 0, false, false); break;
                    case 0x02: branch((int64_t)R(rs) < 0, true, false); break;
                    case 0x03: branch((int64_t)R(rs) >= 0, true, false); break;
                    case 0x10: branch((int64_t)R(rs) < 0, false, true); break;
                    case 0x11: branch((int64_t)R(rs) >= 0, false, true); break;
                    default: unhandled(pc, insn);
                }
                break;
            case 0x02: pending = true; pending_target = (pc & 0xF0000000) | ((insn & 0x3FFFFFF) << 2); break;
            case 0x03: W(31, sx32(pc + 8)); pending = true; pending_target = (pc & 0xF0000000) | ((insn & 0x3FFFFFF) << 2); break;
            case 0x04: branch(R(rs) == R(rt), false, false); break;
            case 0x05: branch(R(rs) != R(rt), false, false); break;
            case 0x06: branch((int64_t)R(rs) <= 0, false, false); break;
            case 0x07: branch((int64_t)R(rs) > 0, false, false); break;
            case 0x08: case 0x09: W(rt, sx32((uint32_t)R(rs) + (uint32_t)simm)); break;
            case 0x0A: W(rt, (int64_t)R(rs) < (int64_t)simm); break;
            case 0x0B: W(rt, R(rs) < (gpr)(int64_t)simm); break;
            case 0x0C: W(rt, R(rs) & uimm); break;
            case 0x0D: W(rt, R(rs) | uimm); break;
            case 0x0E: W(rt, R(rs) ^ uimm); break;
            case 0x0F: W(rt, sx32(uimm << 16)); break;
            case 0x14: branch(R(rs) == R(rt), true, false); break;
            case 0x15: branch(R(rs) != R(rt), true, false); break;
            case 0x16: branch((int64_t)R(rs) <= 0, true, false); break;
            case 0x17: branch((int64_t)R(rs) > 0, true, false); break;
            case 0x20: W(rt, MEM_B(simm, R(rs))); break;
            case 0x21: W(rt, MEM_H(simm, R(rs))); break;
            case 0x23: W(rt, MEM_W(simm, R(rs))); break;
            case 0x24: W(rt, MEM_BU(simm, R(rs))); break;
            case 0x25: W(rt, MEM_HU(simm, R(rs))); break;
            case 0x28: MEM_B(simm, R(rs)) = (int8_t)R(rt); break;
            case 0x29: MEM_H(simm, R(rs)) = (int16_t)R(rt); break;
            case 0x2B: MEM_W(simm, R(rs)) = (int32_t)R(rt); break;
            default: unhandled(pc, insn);
        }

        if (was_pending) {
            // The instruction just executed was a delay slot.
            if (was_target == return_sentinel) {
                ctx->r31 = saved_ra;
                return;
            }
            // A jal/jalr to recompiled code outside this routine runs natively and returns here.
            uint32_t prev = pc - 4;
            uint32_t prev_insn = fetch(rdram, prev);
            bool is_call = (prev_insn >> 26) == 0x03 || ((prev_insn >> 26) == 0 && (prev_insn & 63) == 0x09);
            if (is_call && !gauntlet::interp_owns(was_target)) {
                get_function((int32_t)was_target)(rdram, ctx);
                pc = prev + 8;
                continue;
            }
            pc = was_target;
            continue;
        }
        pc = next_pc;
    }
}
