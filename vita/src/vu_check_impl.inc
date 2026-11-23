// Included by vu_check_sisd.cpp / vu_check_simd.cpp: runs one vector-unit instruction form (the ones the audio
// microcode uses) on a given register / accumulator / flag state. VU_NS names the namespace.
#include <algorithm>
#include <cstdint>
#include "librecomp/rsp.hpp"

namespace VU_NS {
#include "librecomp/rsp_vu.hpp"
#include "librecomp/rsp_vu_impl.hpp"

void run_case(int id, uint16_t* st) {
    // st: vd, vs, vt, acch, accm, accl, vcoh, vcol, vcch, vccl (8 lanes each)
    RSP rsp{};
    RSP::r128* regs[10] = { &rsp.vpu.r[0], &rsp.vpu.r[1], &rsp.vpu.r[2], &rsp.vpu.acch, &rsp.vpu.accm,
                            &rsp.vpu.accl, &rsp.vpu.vcoh, &rsp.vpu.vcol, &rsp.vpu.vcch, &rsp.vpu.vccl };
    for (int k = 0; k < 10; k++) for (int i = 0; i < 8; i++) regs[k]->u16(i) = st[k * 8 + i];
    auto& V0 = rsp.vpu.r[0]; auto& V1 = rsp.vpu.r[1]; auto& V2 = rsp.vpu.r[2];
    switch (id) {
        case 0: rsp.VADD<0>(V0, V1, V2); break;
        case 1: rsp.VADD<10>(V0, V1, V2); break;
        case 2: rsp.VADD<11>(V0, V1, V2); break;
        case 3: rsp.VADD<3>(V0, V1, V2); break;
        case 4: rsp.VADD<6>(V0, V1, V2); break;
        case 5: rsp.VADD<8>(V0, V1, V2); break;
        case 6: rsp.VADD<9>(V0, V1, V2); break;
        case 7: rsp.VADDC<10>(V0, V1, V2); break;
        case 8: rsp.VADDC<11>(V0, V1, V2); break;
        case 9: rsp.VADDC<12>(V0, V1, V2); break;
        case 10: rsp.VADDC<13>(V0, V1, V2); break;
        case 11: rsp.VADDC<15>(V0, V1, V2); break;
        case 12: rsp.VADDC<8>(V0, V1, V2); break;
        case 13: rsp.VADDC<9>(V0, V1, V2); break;
        case 14: rsp.VAND<10>(V0, V1, V2); break;
        case 15: rsp.VAND<11>(V0, V1, V2); break;
        case 16: rsp.VAND<8>(V0, V1, V2); break;
        case 17: rsp.VAND<9>(V0, V1, V2); break;
        case 18: rsp.VLT<9>(V0, V1, V2); break;
        case 19: rsp.VMADH<0>(V0, V1, V2); break;
        case 20: rsp.VMADH<10>(V0, V1, V2); break;
        case 21: rsp.VMADH<11>(V0, V1, V2); break;
        case 22: rsp.VMADH<12>(V0, V1, V2); break;
        case 23: rsp.VMADH<13>(V0, V1, V2); break;
        case 24: rsp.VMADH<14>(V0, V1, V2); break;
        case 25: rsp.VMADH<15>(V0, V1, V2); break;
        case 26: rsp.VMADH<4>(V0, V1, V2); break;
        case 27: rsp.VMADH<8>(V0, V1, V2); break;
        case 28: rsp.VMADH<9>(V0, V1, V2); break;
        case 29: rsp.VMADM<8>(V0, V1, V2); break;
        case 30: rsp.VMADM<9>(V0, V1, V2); break;
        case 31: rsp.VMADN<0>(V0, V1, V2); break;
        case 32: rsp.VMADN<4>(V0, V1, V2); break;
        case 33: rsp.VMADN<8>(V0, V1, V2); break;
        case 34: rsp.VMOV<12>(V0, 0, V2); break;
        case 35: rsp.VMOV<12>(V0, 1, V2); break;
        case 36: rsp.VMOV<12>(V0, 2, V2); break;
        case 37: rsp.VMOV<12>(V0, 3, V2); break;
        case 38: rsp.VMOV<12>(V0, 4, V2); break;
        case 39: rsp.VMOV<12>(V0, 5, V2); break;
        case 40: rsp.VMOV<12>(V0, 6, V2); break;
        case 41: rsp.VMOV<12>(V0, 7, V2); break;
        case 42: rsp.VMRG<0>(V0, V1, V2); break;
        case 43: rsp.VMUDH<14>(V0, V1, V2); break;
        case 44: rsp.VMUDH<4>(V0, V1, V2); break;
        case 45: rsp.VMUDH<8>(V0, V1, V2); break;
        case 46: rsp.VMUDH<9>(V0, V1, V2); break;
        case 47: rsp.VMUDL<13>(V0, V1, V2); break;
        case 48: rsp.VMUDL<9>(V0, V1, V2); break;
        case 49: rsp.VMUDM<10>(V0, V1, V2); break;
        case 50: rsp.VMUDN<0>(V0, V1, V2); break;
        case 51: rsp.VMUDN<11>(V0, V1, V2); break;
        case 52: rsp.VMUDN<12>(V0, V1, V2); break;
        case 53: rsp.VMUDN<4>(V0, V1, V2); break;
        case 54: rsp.VMUDN<8>(V0, V1, V2); break;
        case 55: rsp.VMULF<0>(V0, V1, V2); break;
        case 56: rsp.VMULF<9>(V0, V1, V2); break;
        case 57: rsp.VSAR<10>(V0, V1); break;
        case 58: rsp.VSAR<8>(V0, V1); break;
        case 59: rsp.VSAR<9>(V0, V1); break;
        case 60: rsp.VSUB<9>(V0, V1, V2); break;
        case 61: rsp.VXOR<0>(V0, V1, V2); break;
        case 62: rsp.VADD<2>(V0, V1, V2); break;
        case 63: rsp.VADD<5>(V0, V1, V2); break;
        case 64: rsp.VADD<7>(V0, V1, V2); break;
    }
    for (int k = 0; k < 10; k++) for (int i = 0; i < 8; i++) st[k * 8 + i] = regs[k]->u16(i);
}
}
namespace VU_NS { bool uses_simd() { return Accuracy::RSP::SIMD; } }
