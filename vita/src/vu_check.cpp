// Start-up check of the NEON (sse2neon) vector-unit path against the scalar one, for every instruction form the
// audio microcode uses: random registers, accumulators and flags; results go to log.txt.
#include <cstdint>
#include <cstdio>
#include <random>
#include "vita_log.h"

namespace vu_sisd { void run_case(int id, uint16_t* st); bool uses_simd(); }
namespace vu_simd { void run_case(int id, uint16_t* st); bool uses_simd(); }

static const char* const vu_case_names[] = {
    "VADD<0>",
    "VADD<10>",
    "VADD<11>",
    "VADD<3>",
    "VADD<6>",
    "VADD<8>",
    "VADD<9>",
    "VADDC<10>",
    "VADDC<11>",
    "VADDC<12>",
    "VADDC<13>",
    "VADDC<15>",
    "VADDC<8>",
    "VADDC<9>",
    "VAND<10>",
    "VAND<11>",
    "VAND<8>",
    "VAND<9>",
    "VLT<9>",
    "VMADH<0>",
    "VMADH<10>",
    "VMADH<11>",
    "VMADH<12>",
    "VMADH<13>",
    "VMADH<14>",
    "VMADH<15>",
    "VMADH<4>",
    "VMADH<8>",
    "VMADH<9>",
    "VMADM<8>",
    "VMADM<9>",
    "VMADN<0>",
    "VMADN<4>",
    "VMADN<8>",
    "VMOV<12/0>",
    "VMOV<12/1>",
    "VMOV<12/2>",
    "VMOV<12/3>",
    "VMOV<12/4>",
    "VMOV<12/5>",
    "VMOV<12/6>",
    "VMOV<12/7>",
    "VMRG<0>",
    "VMUDH<14>",
    "VMUDH<4>",
    "VMUDH<8>",
    "VMUDH<9>",
    "VMUDL<13>",
    "VMUDL<9>",
    "VMUDM<10>",
    "VMUDN<0>",
    "VMUDN<11>",
    "VMUDN<12>",
    "VMUDN<4>",
    "VMUDN<8>",
    "VMULF<0>",
    "VMULF<9>",
    "VSAR<10>",
    "VSAR<8>",
    "VSAR<9>",
    "VSUB<9>",
    "VXOR<0>",
    "VADD<2>", "VADD<5>", "VADD<7>"
};

void vu_check() {
    const int ncases = sizeof(vu_case_names) / sizeof(vu_case_names[0]);
    std::mt19937 rng(42);
    int bad_cases = 0;
    for (int id = 0; id < ncases; id++) {
        int bad = 0;
        for (int t = 0; t < 3000; t++) {
            uint16_t a[80], b[80];
            for (int i = 0; i < 48; i++) {
                uint32_t r = rng();
                // Mix in edge values: 0, -1, min, max.
                static const uint16_t edge[4] = { 0x0000, 0xFFFF, 0x8000, 0x7FFF };
                a[i] = (r >> 28) < 3 ? edge[(r >> 16) & 3] : (uint16_t)r;
            }
            for (int i = 48; i < 80; i++) a[i] = (rng() & 1) ? 0xFFFF : 0; // flags are lane masks
            for (int i = 0; i < 80; i++) b[i] = a[i];
            vu_sisd::run_case(id, a);
            vu_simd::run_case(id, b);
            for (int i = 0; i < 80; i++)
                if (a[i] != b[i]) {
                    if (bad == 0)
                        vita_log("vu check: %s differs (word %d: scalar %04X simd %04X)\n", vu_case_names[id], i, a[i], b[i]);
                    bad++;
                    break;
                }
        }
        if (bad) bad_cases++;
    }
    vita_log("vu check: %d instruction forms, %d differ (paths: %s vs %s)\n", ncases, bad_cases,
        vu_sisd::uses_simd() ? "simd" : "scalar", vu_simd::uses_simd() ? "simd" : "scalar");
}
