// F3DEX2 / RDP renderer core (see n64gfx.h).
//
// Pass scheme. The colour combiner runs per vertex on the CPU as a linear form in the texel T:
//     rgb = a(v) * T + b(v)        alpha = aa(v) * Ta + ba(v)
// (TEXEL1 is treated as TEXEL0, T*T as T; fog is folded in as a(1-f), b(1-f) + fog*f). An N64 draw then becomes:
//   opaque, no texture       colour pass b
//   opaque, a uniform        texture x tint(a) pass [+ additive colour pass b]
//   opaque, a varies         colour pass a, texture pass blended DST_COLOR x ZERO, [+ additive colour pass b]
//   translucent              texture x tint pass with the draw's average colour (or a colour pass if untextured)
//   cutout, a uniform        texture x tint pass blended SRC_ALPHA / 1-SRC_ALPHA
//   cutout, a varies         alpha-only pass writing Ta to the target's alpha, colour pass a blended
//                            DST_ALPHA / 1-DST_ALPHA, premultiplied texture pass blended DST_COLOR / 1-SRC_ALPHA
// Follow-up passes test depth with LEQUAL against what the first pass wrote; batches without a depth test are
// split per triangle so a pass never lands on a pixel twice.
//
// Positions: the CPU computes N64 clip space (X, Y, Z, W) and the homogeneous screen position; the vertex input is
// (x', y', W) with x' = ndc_x * W, and the per-batch matrix rebuilds depth as alpha * W + beta, which is exact for
// any perspective projection (z_clip and w_clip both depend only on eye z). Orthographic batches pass Z instead.
#include "n64gfx.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_set>

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

namespace n64gfx {

namespace {
    constexpr uint32_t ADDR_MASK = 0x7FFFFF;

    // Geometry mode (F3DEX2)
    constexpr uint32_t G_ZBUFFER = 0x1, G_SHADE = 0x4, G_CULL_FRONT = 0x200, G_CULL_BACK = 0x400, G_FOG = 0x10000,
        G_LIGHTING = 0x20000, G_TEXTURE_GEN = 0x40000, G_SHADING_SMOOTH = 0x200000;
    // Other mode L
    constexpr uint32_t Z_CMP = 0x10, Z_UPD = 0x20, CVG_X_ALPHA = 0x1000, ALPHA_CVG_SEL = 0x2000, FORCE_BL = 0x4000;
    enum { CYC_1, CYC_2, CYC_COPY, CYC_FILL };

    struct Mtx { float m[4][4]; };

    void mul(const Mtx& a, const Mtx& b, Mtx& out) {
        Mtx r;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        out = r;
    }

    struct Tile {
        uint8_t fmt = 0, siz = 0, palette = 0, cms = 0, cmt = 0, masks = 0, maskt = 0, shifts = 0, shiftt = 0;
        uint16_t line = 0, tmem = 0;
        uint16_t uls = 0, ult = 0, lrs = 0, lrt = 0; // 10.2
    };

    // Vertex in the RSP vertex buffer.
    struct RV {
        float x, y, z, w;   // clip
        float px, py;       // screen position times w
        float s, t;         // texel units (scale applied)
        float c[4];         // shade, 0..1
        uint8_t zm;         // depth mode index
        uint32_t cc_ver;    // combiner state the cached colour forms below were made with (0 = none)
        uint32_t ca, cb;
    };

    struct ZMode { bool ortho; float alpha, beta, wc; };

    // Linear form in the texel.
    struct L { float a, b; };

    enum DrawMode : uint8_t { DM_OPAQUE, DM_XLU, DM_CUTOUT };

    struct Config {
        const TexImage* tex = nullptr;
        bool linear = false;
        Wrap ws = W_CLAMP, wt = W_CLAMP;
        DrawMode mode = DM_OPAQUE;
        Factor bsrc = F_SRC_ALPHA, bdst = F_INV_SRC_ALPHA;
        bool ztest = false, zwrite = false, decal = false;
        bool zcut = false;              // a cutout that updates depth: written afterwards by an alpha-tested pass
        uint8_t zm = 0;
        int sc[4] = {};
        uint32_t target = 0;
        int tile = 0;
        bool operator==(const Config& o) const {
            return tex == o.tex && linear == o.linear && ws == o.ws && wt == o.wt && mode == o.mode && bsrc == o.bsrc &&
                bdst == o.bdst && ztest == o.ztest && zwrite == o.zwrite && decal == o.decal && zcut == o.zcut && zm == o.zm &&
                memcmp(sc, o.sc, sizeof(sc)) == 0 && target == o.target;
        }
    };

    // Batched vertex: position input, uv, and the per-vertex linear colour forms (rgb a, alpha aa / rgb b, alpha ba).
    // ca / cb: the linear colour forms quantised once to RGBA8 (bytes r, g, b, a in memory, as in Vertex).
    struct BV { float x, y, z, u, v; uint32_t ca, cb; };
    // Unquantised colour forms from the combiner.
    struct VC { float ca[4], cb[4]; };

    uint8_t to8(float f) { return f <= 0 ? 0 : f >= 1 ? 255 : (uint8_t)(f * 255.0f + 0.5f); }
    uint32_t pack(float r, float g, float b, float a) { return to8(r) | to8(g) << 8 | to8(b) << 16 | (uint32_t)to8(a) << 24; }
    float clamp01(float f) { return f < 0 ? 0 : f > 1 ? 1 : f; }
    float ch8(uint32_t c, int ch) { return ((c >> (ch * 8)) & 0xFF) / 255.0f; }
}


volatile uint8_t prof_zone = Z_NONE;
const char* const prof_zone_names[Z_COUNT] = { "idle", "parse", "triangles", "vertices", "matrices", "flush", "emit",
    "gpu draws", "soft draws", "textures", "tmem loads", "rects", "targets", "present", "config", "combiner", "lighting" };
namespace {
    struct Zone {
        uint8_t prev;
        explicit Zone(uint8_t z) : prev(prof_zone) { prof_zone = z; }
        ~Zone() { prof_zone = prev; }
    };
}

struct Renderer::Impl {
    Renderer* self;
    uint8_t* rdram;
    Backend* gpu;
    SoftBackend soft;

    // RSP
    uint32_t seg[16] = {};
    Mtx mv_stack[32];
    int mv_sp = 0;
    Mtx mv, proj, mvp;
    bool mvp_dirty = true;
    uint8_t cur_zm = 0;
    // Bumped whenever an input of vertex_colour changes (combiner, colours, cycle type, fog), so each vertex is run
    // through the combiner once per state instead of once per triangle corner.
    uint32_t cc_ver = 1;
    // Combiner results by 8-bit shade (the RDP's precision) under the current state: many vertices share a colour.
    struct ShadeMemo { uint32_t ver, shade, ca, cb; };
    ShadeMemo shade_memo[64] = {};
    std::vector<ZMode> zmodes;
    RV vbuf[64];
    uint32_t geom = 0;
    int num_lights = 0;
    float light_col[8][3] = {}, light_dir[8][3] = {};
    float tex_scale_s = 1, tex_scale_t = 1;
    int tex_tile = 0;
    bool tex_on = false;
    float vp_scale[3] = { 160, 120, 511 }, vp_trans[3] = { 160, 120, 511 };
    float fog_mul = 0, fog_off = 0;

    // RDP
    uint32_t omh = 0, oml = 0, cc0 = 0, cc1 = 0;
    float prim[4] = {}, env[4] = {}, fogc[4] = {}, blendc[4] = {};
    uint32_t fill = 0;
    float prim_lod = 0;
    float key_center[3] = {}, key_scale[3] = {};
    uint32_t timg_addr = 0, timg_width = 0;
    uint8_t timg_siz = 0;
    uint32_t cimg_addr = 0, cimg_width = 320, zimg_addr = 0;
    uint8_t cimg_siz = 2;
    Tile tiles[8];
    alignas(8) uint8_t tmem[4096] = {};
    uint16_t tlut[256] = {};
    float sc_ulx = 0, sc_uly = 0, sc_lrx = 320, sc_lry = 240;
    bool pending_zclear = false;

    // Targets
    std::unordered_set<uint32_t> displayed;          // colour images the VI has shown
    std::unordered_map<uint32_t, int> target_height;  // N64 rows per colour image
    std::unordered_set<uint32_t> gpu_drawn;
    Backend* cur_backend = nullptr;
    uint32_t cur_target = 0;
    std::array<uint32_t, 3> br_ring[8] = {}; // last G_DL commands of the list being run: (address, w1, target)
    int br_count = 0;
    int cur_tw = 0, cur_th = 0;
    float cur_kx = 0, cur_ky = 0; // 2 / cur_tw, 2 / cur_th

    // Textures
    std::unordered_map<uint64_t, TexImage*> tex_cache;
    uint32_t next_tex_id = 1;
    uint32_t frame = 0;

    // Batch
    bool cfg_dirty = true;      // RDP/RSP state changed since tri_cfg was computed
    bool tri_cfg_valid = false;
    Config tri_cfg;
    Config cfg;
    bool have_cfg = false;
    bool cfg_from_tri = false;
    int cur_path = 0;  // cfg is tri_cfg with only zm possibly different
    float tri_ku = 0, tri_kv = 0, tri_cu = 0, tri_cv = 0; // tri_cfg's uv mapping: u = s * ku - cu
    // select_target fast path: the colour image and scissor bottom it last accepted.
    uint32_t sel_addr = 0, sel_width = 0;
    float sel_lry = -1;
    // Texture lookups for unchanged TMEM / tiles skip hashing: tex_ver changes with every TMEM or tile command.
    struct TexMemo { uint32_t ver; int tlut; TexImage* img; Wrap ws, wt; int w, h; };
    TexMemo tex_memo[16] = {};
    uint32_t tex_ver = 1;
    std::vector<BV> batch;
    std::vector<Vertex> vout;
    FILE* trace = nullptr;

    Impl(Renderer* r, uint8_t* rd, Backend* g, bool to_rdram) : self(r), rdram(rd), gpu(g), soft(rd, to_rdram) {
        batch.reserve(4096);
        memset(&mv, 0, sizeof(mv));
        for (int i = 0; i < 4; i++) mv.m[i][i] = 1;
        proj = mv;
        mvp = mv;
    }

    // ---- RDRAM access (words are host order; halfword at o lives at o ^ 2, byte at o ^ 3) ----
    uint32_t rd32(uint32_t a) const { return *(const uint32_t*)(rdram + (a & ADDR_MASK & ~3u)); }
    uint16_t rd16(uint32_t a) const { return *(const uint16_t*)(rdram + ((a & ADDR_MASK & ~1u) ^ 2)); }
    uint8_t rd8(uint32_t a) const { return rdram[(a & ADDR_MASK) ^ 3]; }

    // TMEM stores: the 4 / 2 N64-order bytes of the RDRAM word at a (4-aligned).
    void tmem_put4(uint32_t o, uint32_t a) {
        uint32_t w = __builtin_bswap32(rd32(a));
        memcpy(tmem + (o & 0xFFC), &w, 4);
    }
    void tmem_put32_texel(uint32_t o, uint32_t a) { // RGBA32: R, G to the low half, B, A to the high half
        uint32_t w = rd32(a);
        tmem[o] = (uint8_t)(w >> 24); tmem[o + 1] = (uint8_t)(w >> 16);
        tmem[0x800 + o] = (uint8_t)(w >> 8); tmem[0x801 + o] = (uint8_t)w;
    }
    uint32_t segaddr(uint32_t a) const { return (seg[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & ADDR_MASK; }

    static void color(uint32_t w, float* c) {
        c[0] = (w >> 24) / 255.0f; c[1] = ((w >> 16) & 0xFF) / 255.0f; c[2] = ((w >> 8) & 0xFF) / 255.0f; c[3] = (w & 0xFF) / 255.0f;
    }

    // ---- Matrices ----
    void load_mtx(uint32_t addr, Mtx& out) {
        for (int k = 0; k < 16; k++) {
            int32_t v = (int32_t)(((uint32_t)rd16(addr + k * 2) << 16) | rd16(addr + 32 + k * 2));
            out.m[k / 4][k % 4] = v / 65536.0f;
        }
    }

    void update_mvp() {
        Zone zone_(Z_MTX);
        if (!mvp_dirty) return;
        mul(mv, proj, mvp);
        mvp_dirty = false;
        // Depth relation of this matrix: z_clip = alpha * w_clip + beta, or orthographic (w constant).
        ZMode z{};
        float cz[4] = { mvp.m[0][2], mvp.m[1][2], mvp.m[2][2], mvp.m[3][2] };
        float cw[4] = { mvp.m[0][3], mvp.m[1][3], mvp.m[2][3], mvp.m[3][3] };
        int k = 0;
        for (int i = 1; i < 3; i++) if (fabsf(cw[i]) > fabsf(cw[k])) k = i;
        float big = std::max({ fabsf(cz[0]), fabsf(cz[1]), fabsf(cz[2]), 1e-12f });
        if (fabsf(cw[k]) < 1e-6f * big || fabsf(cw[k]) < 1e-9f) {
            z.ortho = true;
            z.wc = cw[3] != 0 ? cw[3] : 1;
        } else {
            z.alpha = cz[k] / cw[k];
            z.beta = cz[3] - z.alpha * cw[3];
        }
        for (size_t i = 0; i < zmodes.size(); i++) {
            const ZMode& o = zmodes[i];
            if (o.ortho == z.ortho && fabsf(o.alpha - z.alpha) <= 1e-5f * (1 + fabsf(z.alpha)) &&
                fabsf(o.beta - z.beta) <= 1e-5f * (1 + fabsf(z.beta)) && o.wc == z.wc) {
                cur_zm = (uint8_t)i;
                return;
            }
        }
        if (zmodes.size() < 255) zmodes.push_back(z);
        cur_zm = (uint8_t)(zmodes.size() - 1);
    }

    void wvp_for(uint8_t zm, float* f) const {
        memset(f, 0, 16 * sizeof(float));
        const ZMode& z = zmodes[zm];
        f[0] = 1;
        f[5] = 1;
        // N64 depth z/w in [-1, 1] maps to [0, 0.5]: the RSP does not clip at the far plane, so geometry up to
        // z/w = 3 stays inside the [0, 1] range GPUs clip to.
        if (z.ortho) {          // input (x', y', Z): depth (Z + W) / 4, w = W
            f[10] = 0.25f;
            f[14] = 0.25f * z.wc;
            f[15] = z.wc;
        } else {                // input (x', y', W): depth (alpha W + beta + W) / 4, w = W
            f[10] = 0.25f * z.alpha + 0.25f;
            f[14] = 0.25f * z.beta;
            f[11] = 1;
        }
    }

    // ---- Vertices ----
    void load_vertices(uint32_t addr, int n, int v0) {
        Zone zone_(Z_VTX);
        update_mvp();
        const Mtx& m = mvp;
        float nrm[8][3];
        bool lighting = (geom & G_LIGHTING) != 0;
        if (lighting) {
            for (int l = 0; l < num_lights; l++) {
                float* d = light_dir[l];
                float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                for (int c = 0; c < 3; c++) nrm[l][c] = len > 0 ? d[c] / len : 0;
            }
        }
        for (int i = 0; i < n && v0 + i < 64; i++) {
            // N64 Vtx: x y | z flag | s t | r g b a (or nx ny nz a), as four big-endian words.
            uint32_t a = addr + i * 16;
            uint32_t q0 = rd32(a), q1 = rd32(a + 4), q2 = rd32(a + 8), q3 = rd32(a + 12);
            float x = (int16_t)(q0 >> 16), y = (int16_t)q0, z = (int16_t)(q1 >> 16);
            RV& v = vbuf[v0 + i];
            v.x = x * m.m[0][0] + y * m.m[1][0] + z * m.m[2][0] + m.m[3][0];
            v.y = x * m.m[0][1] + y * m.m[1][1] + z * m.m[2][1] + m.m[3][1];
            v.z = x * m.m[0][2] + y * m.m[1][2] + z * m.m[2][2] + m.m[3][2];
            v.w = x * m.m[0][3] + y * m.m[1][3] + z * m.m[2][3] + m.m[3][3];
            v.px = v.x * vp_scale[0] + v.w * vp_trans[0];
            v.py = -v.y * vp_scale[1] + v.w * vp_trans[1];
            v.s = (int16_t)(q2 >> 16) * tex_scale_s / 32.0f;
            v.t = (int16_t)q2 * tex_scale_t / 32.0f;
            v.zm = cur_zm;
            v.cc_ver = 0;
            uint8_t cr = (uint8_t)(q3 >> 24), cg = (uint8_t)(q3 >> 16), cb = (uint8_t)(q3 >> 8), ca = (uint8_t)q3;
            if (lighting) {
                prof_zone = Z_LIGHT;
                float nx = (int8_t)cr, ny = (int8_t)cg, nz = (int8_t)cb;
                float ex = nx * mv.m[0][0] + ny * mv.m[1][0] + nz * mv.m[2][0];
                float ey = nx * mv.m[0][1] + ny * mv.m[1][1] + nz * mv.m[2][1];
                float ez = nx * mv.m[0][2] + ny * mv.m[1][2] + nz * mv.m[2][2];
                float len2 = ex * ex + ey * ey + ez * ez;
                if (len2 > 0) { float il = 1.0f / sqrtf(len2); ex *= il; ey *= il; ez *= il; }
                float col[3] = { light_col[num_lights][0], light_col[num_lights][1], light_col[num_lights][2] };
                for (int l = 0; l < num_lights; l++) {
                    float d = ex * nrm[l][0] + ey * nrm[l][1] + ez * nrm[l][2];
                    if (d > 0) for (int c = 0; c < 3; c++) col[c] += light_col[l][c] * d;
                }
                for (int c = 0; c < 3; c++) v.c[c] = clamp01(col[c]);
                prof_zone = Z_VTX;
            } else {
                v.c[0] = cr / 255.0f; v.c[1] = cg / 255.0f; v.c[2] = cb / 255.0f;
            }
            v.c[3] = ca / 255.0f;
            if (geom & G_FOG) {
                float zw = v.w > 0 ? v.z / v.w : (v.z < 0 ? -1.0f : 1.0f);
                v.c[3] = clamp01((zw * fog_mul + fog_off) / 255.0f);
            }
        }
    }

    // ---- Combiner ----
    // rgb input for slot kind (0 = A, 1 = B, 2 = C, 3 = D) and selector; alpha input likewise.
    L cin(int kind, int sel, int ch, const float* shade, const L* comb, const L& comb_a) const {
        auto k = [](float b) { return L{ 0, b }; };
        switch (sel) {
        case 0: return comb ? comb[ch] : k(0);
        case 1: case 2: return L{ 1, 0 };
        case 3: return k(prim[ch]);
        case 4: return k(shade[ch]);
        case 5: return k(env[ch]);
        }
        if (kind == 0) return k(sel == 6 ? 1.0f : sel == 7 ? 0.5f : 0.0f);
        if (kind == 1) return k(sel == 6 ? key_center[ch] : 0.0f);
        if (kind == 3) return k(sel == 6 ? 1.0f : 0.0f);
        switch (sel) { // C
        case 6: return k(key_scale[ch]);
        case 7: return k(comb ? comb_a.a + comb_a.b : 0);
        case 8: case 9: return k(1);
        case 10: return k(prim[3]);
        case 11: return k(shade[3]);
        case 12: return k(env[3]);
        case 14: return k(prim_lod);
        default: return k(0);
        }
    }

    L ain(int kind, int sel, const float* shade, const L* comb) const {
        auto k = [](float b) { return L{ 0, b }; };
        switch (sel) {
        case 0: return kind == 2 ? k(0) : (comb ? *comb : k(0));
        case 1: case 2: return L{ 1, 0 };
        case 3: return k(prim[3]);
        case 4: return k(shade[3]);
        case 5: return k(env[3]);
        case 6: return k(kind == 2 ? prim_lod : 1.0f);
        default: return k(0);
        }
    }

    static L lmul(L x, L y) { return L{ x.a * y.a + x.a * y.b + y.a * x.b, x.b * y.b }; }

    // Runs both cycles of the combiner for one vertex: out_rgb[3], out_a.
    void combine(const float* shade, int cycles, L* out_rgb, L& out_a) const {
        L comb[3], comb_a{};
        bool have = false;
        for (int c = 0; c < cycles; c++) {
            int a, b, cc, d, aa, ab, ac, ad;
            if (c == 0) {
                a = (cc0 >> 20) & 0xF; cc = (cc0 >> 15) & 0x1F; aa = (cc0 >> 12) & 7; ac = (cc0 >> 9) & 7;
                b = (cc1 >> 28) & 0xF; d = (cc1 >> 15) & 7; ab = (cc1 >> 12) & 7; ad = (cc1 >> 9) & 7;
            } else {
                a = (cc0 >> 5) & 0xF; cc = cc0 & 0x1F; aa = (cc1 >> 21) & 7; ac = (cc1 >> 18) & 7;
                b = (cc1 >> 24) & 0xF; d = (cc1 >> 6) & 7; ab = (cc1 >> 3) & 7; ad = cc1 & 7;
            }
            const L* pc = have ? comb : nullptr;
            L r[3];
            for (int ch = 0; ch < 3; ch++) {
                L A = cin(0, a, ch, shade, pc, comb_a), B = cin(1, b, ch, shade, pc, comb_a);
                L C = cin(2, cc, ch, shade, pc, comb_a), D = cin(3, d, ch, shade, pc, comb_a);
                L s{ A.a - B.a, A.b - B.b };
                L m = lmul(s, C);
                r[ch] = L{ m.a + D.a, m.b + D.b };
            }
            L A = ain(0, aa, shade, have ? &comb_a : nullptr), B = ain(1, ab, shade, have ? &comb_a : nullptr);
            L C = ain(2, ac, shade, have ? &comb_a : nullptr), D = ain(3, ad, shade, have ? &comb_a : nullptr);
            L m = lmul(L{ A.a - B.a, A.b - B.b }, C);
            comb_a = L{ m.a + D.a, m.b + D.b };
            for (int ch = 0; ch < 3; ch++) comb[ch] = r[ch];
            have = true;
        }
        for (int ch = 0; ch < 3; ch++) out_rgb[ch] = comb[ch];
        out_a = comb_a;
    }

    int cycle_type() const { return (omh >> 20) & 3; }

    // ---- Textures ----
    // TMEM or a tile is about to change: draw what used the old state first.
    void tmem_state_change() {
        flush();
        if (++tex_ver == 0) tex_ver = 1;
        cfg_dirty = true;
    }

    void set_tile(int idx, const Tile& nt) {
        if (memcmp(&tiles[idx], &nt, sizeof(Tile)) == 0) return;
        tmem_state_change();
        tiles[idx] = nt;
    }

    void load_block(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
        Zone zone_(Z_TMEM);
        Tile nt = tiles[tile];
        nt.uls = uls << 2; nt.ult = ult << 2; nt.lrs = lrs << 2; nt.lrt = ult << 2;
        uint32_t count = lrs - uls + 1;
        uint32_t base = nt.tmem * 8;
        uint32_t src = timg_addr + (((ult * timg_width + uls) << timg_siz) >> 1);
        tmem_state_change();
        tiles[tile] = nt;
        // Word-at-a-time copies when the source is word aligned (always, in practice); bytewise otherwise.
        bool aligned = (src & 3) == 0;
        if (timg_siz == 3) {
            for (uint32_t k = 0; k < count && k < 1024; k++) {
                uint32_t o = (base + k * 2) & 0x7FF;
                if (aligned) { tmem_put32_texel(o, src + k * 4); continue; }
                tmem[o] = rd8(src + k * 4); tmem[o + 1] = rd8(src + k * 4 + 1);
                tmem[0x800 + o] = rd8(src + k * 4 + 2); tmem[0x801 + o] = rd8(src + k * 4 + 3);
            }
        } else {
            uint32_t bytes = (count << timg_siz) >> 1;
            uint32_t words = std::min<uint32_t>((bytes + 7) / 8, 512);
            if (aligned && base + words * 8 <= 4096 && (src & ADDR_MASK) + words * 8 <= ADDR_MASK + 1) {
                // Each RDRAM word holds 4 big-endian bytes: one byte swap per word; odd lines swap the word pair.
                const uint32_t* sp = (const uint32_t*)(rdram + (src & ADDR_MASK));
                uint32_t* dp = (uint32_t*)(tmem + base);
                for (uint32_t i = 0; i < words; i++) {
                    uint32_t a = __builtin_bswap32(sp[2 * i]), b = __builtin_bswap32(sp[2 * i + 1]);
                    bool odd = dxt && (((i * dxt) >> 11) & 1);
                    dp[2 * i] = odd ? b : a;
                    dp[2 * i + 1] = odd ? a : b;
                }
                return;
            }
            for (uint32_t i = 0; i < words; i++) {
                uint32_t line = dxt ? (i * dxt) >> 11 : 0;
                uint32_t x = (line & 1) ? 4 : 0;
                if (aligned) {
                    tmem_put4((base + i * 8) ^ x, src + i * 8);
                    tmem_put4((base + i * 8 + 4) ^ x, src + i * 8 + 4);
                    continue;
                }
                for (uint32_t b = 0; b < 8; b++) tmem[((base + i * 8 + b) ^ x) & 0xFFF] = rd8(src + i * 8 + b);
            }
        }
    }

    void load_tile(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
        Zone zone_(Z_TMEM);
        Tile nt = tiles[tile];
        nt.uls = uls; nt.ult = ult; nt.lrs = lrs; nt.lrt = lrt;
        uint32_t x0 = uls >> 2, y0 = ult >> 2, x1 = lrs >> 2, y1 = lrt >> 2;
        if (x1 < x0 || y1 < y0) { set_tile(tile, nt); return; }
        uint32_t base = nt.tmem * 8, line = nt.line * 8;
        tmem_state_change();
        tiles[tile] = nt;
        for (uint32_t y = y0; y <= y1 && y - y0 < 1024; y++) {
            uint32_t r = y - y0;
            uint32_t src = timg_addr + (((y * timg_width + x0) << timg_siz) >> 1);
            if (timg_siz == 3) {
                bool aligned = (src & 3) == 0;
                for (uint32_t k = 0; k <= x1 - x0; k++) {
                    uint32_t o = (base + r * line + k * 2) & 0x7FF;
                    if (aligned) { tmem_put32_texel(o, src + k * 4); continue; }
                    tmem[o] = rd8(src + k * 4); tmem[o + 1] = rd8(src + k * 4 + 1);
                    tmem[0x800 + o] = rd8(src + k * 4 + 2); tmem[0x801 + o] = rd8(src + k * 4 + 3);
                }
            } else {
                uint32_t bytes = std::min<uint32_t>(((x1 - x0 + 1) << timg_siz) >> 1, 4096);
                uint32_t x = (r & 1) ? 4 : 0;
                uint32_t d = base + r * line;
                uint32_t b = 0;
                if ((src & 3) == 0 && (d & 3) == 0)
                    for (; b + 4 <= bytes; b += 4) tmem_put4((d + b) ^ x, src + b);
                for (; b < bytes; b++) tmem[((d + b) ^ x) & 0xFFF] = rd8(src + b);
            }
        }
    }

    void load_tlut(int tile, uint32_t uls, uint32_t ult, uint32_t lrs) {
        Zone zone_(Z_TMEM);
        int count = (int)(lrs >> 2) - (int)(uls >> 2) + 1;
        int base = (int)tiles[tile].tmem - 256;
        if (base < 0) base = 0;
        uint32_t src = timg_addr + (((ult >> 2) * timg_width + (uls >> 2)) << 1);
        bool changed = false;
        for (int i = 0; i < count && i < 256 && !changed; i++) changed = tlut[(base + i) & 255] != rd16(src + i * 2);
        if (!changed) return;
        tmem_state_change();
        for (int i = 0; i < count && i < 256; i++) tlut[(base + i) & 255] = rd16(src + i * 2);
    }

    static uint32_t rgba16(uint16_t c) {
        uint32_t r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
        r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2;
        return r | g << 8 | b << 16 | ((c & 1) ? 0xFF000000u : 0);
    }
    static uint32_t ia16(uint16_t c) { uint32_t i = c >> 8; return i | i << 8 | i << 16 | (uint32_t)(c & 0xFF) << 24; }

    // Size and wrap of one axis of a tile.
    static void axis(int lo, int hi, int mask, int cm, int& size, Wrap& wrap) {
        int ts = ((hi - lo) >> 2) + 1;
        if (ts < 1) ts = 1;
        if (mask) {
            int period = 1 << std::min(mask, 10);
            if ((cm & 2) && ts <= period) { size = ts; wrap = W_CLAMP; }
            else { size = period; wrap = (cm & 1) ? W_MIRROR : W_REPEAT; }
        } else {
            size = ts;
            wrap = W_CLAMP;
        }
        size = std::min(size, 1024);
    }

    const TexImage* texture(int tile_idx, bool premul, Wrap& ws, Wrap& wt, int& w, int& h) {
        Zone zone_(Z_TEX);
        TexMemo& m = tex_memo[tile_idx + (premul ? 8 : 0)];
        int tlut_type = (omh >> 14) & 3;
        if (m.ver == tex_ver && m.tlut == tlut_type && m.img) {
            m.img->last_used = frame;
            ws = m.ws; wt = m.wt; w = m.w; h = m.h;
            return m.img;
        }
        TexImage* r = texture_uncounted(tile_idx, premul, ws, wt, w, h);
        m = TexMemo{ tex_ver, tlut_type, r, ws, wt, w, h };
        return r;
    }


    TexImage* texture_uncounted(int tile_idx, bool premul, Wrap& ws, Wrap& wt, int& w, int& h) {
        const Tile& t = tiles[tile_idx];
        axis(t.uls, t.lrs, t.masks, t.cms, w, ws);
        axis(t.ult, t.lrt, t.maskt, t.cmt, h, wt);
        uint32_t base = t.tmem * 8, line = t.line * 8;
        if (line == 0) line = 8;
        int tlut_type = (omh >> 14) & 3;
        // Key: tile parameters, the TMEM bytes it covers, and the palette it uses.
        uint32_t params[8] = { t.fmt, t.siz, line, base, (uint32_t)w, (uint32_t)h, (uint32_t)tlut_type | (premul ? 16u : 0u), t.palette };
        XXH3_state_t st;
        XXH3_64bits_reset(&st);
        XXH3_64bits_update(&st, params, sizeof(params));
        uint32_t span = std::min<uint32_t>(line * h + 8, 4096);
        if (t.siz == 3) {
            span = std::min<uint32_t>(span, 2048);
            uint32_t b = base & 0x7FF;
            if (b + span <= 2048) { XXH3_64bits_update(&st, tmem + b, span); XXH3_64bits_update(&st, tmem + 0x800 + b, span); }
            else XXH3_64bits_update(&st, tmem, 4096);
        } else {
            uint32_t b = base & 0xFFF;
            if (b + span <= 4096) XXH3_64bits_update(&st, tmem + b, span);
            else XXH3_64bits_update(&st, tmem, 4096);
        }
        if (t.fmt == 2) {
            if (t.siz == 0) XXH3_64bits_update(&st, tlut + (t.palette & 15) * 16, 32);
            else XXH3_64bits_update(&st, tlut, 512);
        }
        uint64_t key = XXH3_64bits_digest(&st);
        auto it = tex_cache.find(key);
        if (it != tex_cache.end()) {
            it->second->last_used = frame;
            return it->second;
        }
        TexImage* img = new TexImage{ next_tex_id++, w, h, {}, key, frame };
        img->px.resize((size_t)w * h);
        self->stats.tex_decodes++;
        for (int y = 0; y < h; y++) {
            uint32_t row = base + y * line;
            uint32_t sw = (y & 1) ? 4 : 0;
            for (int x = 0; x < w; x++) {
                uint32_t c = 0;
                auto byte = [&](uint32_t off) { return tmem[((row + off) ^ sw) & 0xFFF]; };
                switch (t.siz) {
                case 0: {
                    uint8_t v = byte(x >> 1);
                    uint8_t n = (x & 1) ? (v & 0xF) : (v >> 4);
                    if (t.fmt == 2) { uint16_t p = tlut[((t.palette & 15) << 4) | n]; c = tlut_type == 3 ? ia16(p) : rgba16(p); }
                    else if (t.fmt == 3) { uint32_t i = (n >> 1) * 255 / 7; c = i | i << 8 | i << 16 | ((n & 1) ? 0xFF000000u : 0); }
                    else { uint32_t i = n * 17; c = i | i << 8 | i << 16 | i << 24; }
                    break;
                }
                case 1: {
                    uint8_t v = byte(x);
                    if (t.fmt == 2) { uint16_t p = tlut[v]; c = tlut_type == 3 ? ia16(p) : rgba16(p); }
                    else if (t.fmt == 3) { uint32_t i = (v >> 4) * 17, a = (v & 0xF) * 17; c = i | i << 8 | i << 16 | a << 24; }
                    else { uint32_t i = v; c = i | i << 8 | i << 16 | i << 24; }
                    break;
                }
                case 2: {
                    uint16_t v = (uint16_t)(byte(x * 2) << 8 | byte(x * 2 + 1));
                    c = t.fmt == 3 ? ia16(v) : rgba16(v);
                    break;
                }
                default: {
                    uint32_t o = (row + x * 2) & 0x7FF;
                    c = tmem[o] | tmem[o + 1] << 8 | tmem[0x800 + o] << 16 | (uint32_t)tmem[0x801 + o] << 24;
                    break;
                }
                }
                if (premul) {
                    uint32_t a = c >> 24;
                    c = ((c & 0xFF) * a / 255) | (((c >> 8) & 0xFF) * a / 255) << 8 | (((c >> 16) & 0xFF) * a / 255) << 16 | (c & 0xFF000000u);
                }
                img->px[(size_t)y * w + x] = c;
            }
        }
        tex_cache[key] = img;
        return img;
    }

    void evict_textures() {
        if (++tex_ver == 0) tex_ver = 1;
        for (auto it = tex_cache.begin(); it != tex_cache.end();) {
            if (frame - it->second->last_used > 90) {
                gpu->forget_texture(it->second->id);
                soft.forget_texture(it->second->id);
                delete it->second;
                it = tex_cache.erase(it);
            } else ++it;
        }
        self->stats.textures = (uint32_t)tex_cache.size();
    }

    // ---- Targets ----
    bool select_target() {
        if (cimg_addr == 0) return false;
        if (cimg_addr == sel_addr && cimg_width == sel_width && sc_lry == sel_lry && cur_target == cimg_addr && !pending_zclear)
            return true;
        bool ok = select_target_slow();
        sel_addr = cimg_addr; sel_width = cimg_width; sel_lry = sc_lry;
        return ok;
    }

    bool select_target_slow() {
        Zone zone_(Z_TARGET);
        int h = (int)std::ceil(sc_lry);
        if (h < 1) h = 1;
        auto it = target_height.find(cimg_addr);
        if (it == target_height.end()) target_height[cimg_addr] = h;
        else if (h > it->second) { it->second = h; cur_target = 0; }
        h = target_height[cimg_addr];
        Backend* b = displayed.count(cimg_addr) ? gpu : static_cast<Backend*>(&soft);
        if (cur_target != cimg_addr || cur_backend != b || cur_th != h) {
            flush();
            cur_target = cimg_addr;
            cur_backend = b;
            cur_tw = (int)cimg_width;
            cur_th = h;
            cur_kx = 2.0f / cur_tw;
            cur_ky = 2.0f / cur_th;
            b->begin_target(cimg_addr, cur_tw, cur_th);
            if (b == gpu) gpu_drawn.insert(cimg_addr);
        }
        if (pending_zclear) {
            pending_zclear = false;
            clear_depth();
        }
        return true;
    }

    void base_state(PassState& s, uint8_t zm, const int* sc) const {
        wvp_for(zm, s.wvp);
        s.sx0 = sc[0]; s.sy0 = sc[1]; s.sx1 = sc[2]; s.sy1 = sc[3];
    }

    void scissor(int* sc) const {
        sc[0] = std::max(0, (int)sc_ulx);
        sc[1] = std::max(0, (int)sc_uly);
        sc[2] = std::min(cur_tw, (int)std::ceil(sc_lrx));
        sc[3] = std::min(cur_th, (int)std::ceil(sc_lry));
    }

    uint8_t ortho_zm() {
        for (size_t i = 0; i < zmodes.size(); i++) if (zmodes[i].ortho && zmodes[i].wc == 1) return (uint8_t)i;
        if (zmodes.size() >= 255) return 0;
        zmodes.push_back(ZMode{ true, 0, 0, 1 });
        return (uint8_t)(zmodes.size() - 1);
    }

    // A screen rectangle in target pixels as two triangles of a colour-only draw.
    void rect_vertices(float x0, float y0, float x1, float y1, float z, const float* uv, Vertex* v, uint32_t col) const {
        float nx0 = 2 * x0 / cur_tw - 1, nx1 = 2 * x1 / cur_tw - 1, ny0 = 1 - 2 * y0 / cur_th, ny1 = 1 - 2 * y1 / cur_th;
        float u0 = uv ? uv[0] : 0, v0 = uv ? uv[1] : 0, u1 = uv ? uv[2] : 0, v1 = uv ? uv[3] : 0;
        Vertex q[4] = { { nx0, ny0, z, u0, v0 }, { nx1, ny0, z, u1, v0 }, { nx1, ny1, z, u1, v1 }, { nx0, ny1, z, u0, v1 } };
        for (auto& p : q) { p.r = col & 0xFF; p.g = (col >> 8) & 0xFF; p.b = (col >> 16) & 0xFF; p.a = col >> 24; }
        static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i = 0; i < 6; i++) v[i] = q[idx[i]];
    }

    void clear_depth() {
        PassState s;
        int sc[4] = { 0, 0, cur_tw, cur_th };
        base_state(s, ortho_zm(), sc);
        s.mask = 0;
        s.depth = D_ALWAYS;
        s.depth_write = true;
        Vertex v[6];
        rect_vertices(0, 0, (float)cur_tw, (float)cur_th, 2.99f, nullptr, v, 0); // depth ~1, inside the clip range
        cur_backend->draw(s, v, 6);
        self->stats.passes++;
    }

    // ---- Draw configuration from the RDP state ----
    void compute_config(Config& c, int tile, bool rect) {
        Zone zone_(Z_CFG);
        c = Config{};
        c.target = cur_target;
        c.tile = tile;
        scissor(c.sc);
        int cyc = cycle_type();
        uint32_t l = oml;
        if (cyc == CYC_COPY) {
            c.mode = (l & 3) ? DM_CUTOUT : DM_OPAQUE;
        } else {
            int p, a, m, b;
            if (cyc == CYC_2) { p = (l >> 28) & 3; a = (l >> 24) & 3; m = (l >> 20) & 3; b = (l >> 16) & 3; }
            else { p = (l >> 30) & 3; a = (l >> 26) & 3; m = (l >> 22) & 3; b = (l >> 18) & 3; }
            if ((l & FORCE_BL) && p == 0 && m == 1 && a == 0 && b == 0) c.mode = DM_XLU;
            else if ((l & FORCE_BL) && p == 0 && m == 1 && b == 2) { c.mode = DM_XLU; c.bdst = F_ONE; c.bsrc = a == 3 ? F_ONE : F_SRC_ALPHA; }
            else if ((l & 3) == 1 || (l & 3) == 3 || ((l & ALPHA_CVG_SEL) && (l & CVG_X_ALPHA))) c.mode = DM_CUTOUT;
        }
        if (!rect && (geom & G_ZBUFFER) && cyc < CYC_COPY) {
            c.ztest = (l & Z_CMP) != 0;
            c.zwrite = (l & Z_UPD) != 0 && c.mode != DM_CUTOUT;
            c.decal = ((l >> 10) & 3) == 3;
            c.zcut = (l & Z_UPD) != 0 && c.mode == DM_CUTOUT && !c.decal;
            if (c.decal) c.zwrite = false;
        }
        Wrap ws, wt;
        int w, h;
        c.tex = texture(tile, false, ws, wt, w, h);
        c.ws = ws; c.wt = wt;
        c.linear = cyc != CYC_COPY && ((omh >> 12) & 3) != 0;
    }

    bool fog_on() const {
        // Fog blend in the first blender cycle: fog colour * shade alpha + combined * (1 - shade alpha).
        if (cycle_type() != CYC_2) return false;
        uint32_t l = oml;
        return ((l >> 30) & 3) == 3 && ((l >> 26) & 3) == 2 && (geom & G_FOG);
    }

    // ---- Triangles ----
    // uv_of as u = s * ku - cu, v = t * kv - cv.
    void uv_map(const Config& c, float& ku, float& kv, float& cu, float& cv) const {
        if (!c.tex) { ku = kv = cu = cv = 0; return; }
        const Tile& tl = tiles[c.tile];
        auto scale = [](int sh) { return sh == 0 ? 1.0f : sh <= 10 ? 1.0f / (float)(1 << sh) : (float)(1 << (16 - sh)); };
        ku = scale(tl.shifts) / (float)c.tex->w; cu = tl.uls / 4.0f / (float)c.tex->w;
        kv = scale(tl.shiftt) / (float)c.tex->h; cv = tl.ult / 4.0f / (float)c.tex->h;
    }

    void uv_of(const Config& c, float s, float t, float& u, float& v) const {
        const Tile& tl = tiles[c.tile];
        auto shift = [](float x, int sh) { return sh == 0 ? x : sh <= 10 ? x / (float)(1 << sh) : x * (float)(1 << (16 - sh)); };
        s = shift(s, tl.shifts) - tl.uls / 4.0f;
        t = shift(t, tl.shiftt) - tl.ult / 4.0f;
        u = s / (float)c.tex->w;
        v = t / (float)c.tex->h;
    }

    void vertex_colour(const float* shade, int cycles, bool fog, BV& o) const {
        VC f;
        vertex_colour(shade, cycles, fog, f);
        o.ca = pack(f.ca[0], f.ca[1], f.ca[2], f.ca[3]);
        o.cb = pack(f.cb[0], f.cb[1], f.cb[2], f.cb[3]);
    }

    void vertex_colour(const float* shade, int cycles, bool fog, VC& o) const {
        L rgb[3], al;
        combine(shade, cycles, rgb, al);
        float f = fog ? shade[3] : 0;
        for (int ch = 0; ch < 3; ch++) {
            o.ca[ch] = rgb[ch].a * (1 - f);
            o.cb[ch] = rgb[ch].b * (1 - f) + fogc[ch] * f;
        }
        o.ca[3] = al.a;
        o.cb[3] = al.b;
    }

    void triangle(int i0, int i1, int i2) {
        Zone zone_(Z_TRI);
        if (i0 >= 64 || i1 >= 64 || i2 >= 64) return;
        RV* v[3] = { &vbuf[i0], &vbuf[i1], &vbuf[i2] };
        self->stats.tris++;
        if (v[0]->w > 0 && v[1]->w > 0 && v[2]->w > 0 && (geom & (G_CULL_FRONT | G_CULL_BACK))) {
            // Sign of the screen-space cross product without dividing by w (all w > 0): det |px py w| per vertex.
            float cross = v[0]->px * (v[1]->py * v[2]->w - v[2]->py * v[1]->w)
                        - v[1]->px * (v[0]->py * v[2]->w - v[2]->py * v[0]->w)
                        + v[2]->px * (v[0]->py * v[1]->w - v[1]->py * v[0]->w);
            if ((geom & G_CULL_BACK) && cross > 0) return;
            if ((geom & G_CULL_FRONT) && cross < 0) return;
        }
        if (!select_target()) return;
        if (cycle_type() >= CYC_COPY || v[0]->zm >= zmodes.size()) return;
        if (cfg_dirty || !tri_cfg_valid) {
            compute_config(tri_cfg, tex_tile, false);
            tri_cfg_valid = true;
            cfg_dirty = false;
            cfg_from_tri = false;
            uv_map(tri_cfg, tri_ku, tri_kv, tri_cu, tri_cv);
        }
        if (!(have_cfg && cfg_from_tri && cfg.zm == v[0]->zm)) {
            Config c = tri_cfg;
            c.zm = v[0]->zm;
            use_config(c);
            cfg_from_tri = true;
        }
        const Config& c = cfg;
        int cycles = cycle_type() == CYC_2 ? 2 : 1;
        bool fog = fog_on();
        const ZMode& zm = zmodes[c.zm];
        const float kx = cur_kx, ky = cur_ky;
        for (int k = 0; k < 3; k++) {
            const RV& r = *v[k];
            BV o;
            // In orthographic batches w is the same constant for every vertex.
            o.x = r.px * kx - r.w;
            o.y = r.w - r.py * ky;
            o.z = zm.ortho ? r.z : r.w;
            o.u = r.s * tri_ku - tri_cu;
            o.v = r.t * tri_kv - tri_cv;
            RV& sv = (geom & G_SHADING_SMOOTH) ? *v[k] : *v[0];
            if (sv.cc_ver != cc_ver) {
                BV t;
                uint32_t key = pack(sv.c[0], sv.c[1], sv.c[2], sv.c[3]);
                ShadeMemo& m = shade_memo[(key * 0x9E3779B1u) >> 26];
                if (m.ver == cc_ver && m.shade == key) {
                    t.ca = m.ca; t.cb = m.cb;
                } else {
                    prof_zone = Z_COMB;
                    vertex_colour(sv.c, cycles, fog, t);
                    prof_zone = Z_TRI;
                    m = ShadeMemo{ cc_ver, key, t.ca, t.cb };
                }
                sv.ca = t.ca;
                sv.cb = t.cb;
                sv.cc_ver = cc_ver;
            }
            o.ca = sv.ca;
            o.cb = sv.cb;
            batch.push_back(o);
        }
    }

    void use_config(const Config& c) {
        cfg_from_tri = false;
        if (have_cfg && c == cfg) return;
        flush();
        cfg = c;
        have_cfg = true;
    }

    // ---- Rectangles ----
    void tex_rect(uint32_t w0, uint32_t w1, uint32_t h1, uint32_t h2, bool flip) {
        Zone zone_(Z_RECT);
        if (!select_target()) return;
        int cyc = cycle_type();
        float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
        float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
        int tile = (w1 >> 24) & 7;
        float s = (int16_t)(h1 >> 16) / 32.0f, t = (int16_t)(h1 & 0xFFFF) / 32.0f;
        float dsdx = (int16_t)(h2 >> 16) / 1024.0f, dtdy = (int16_t)(h2 & 0xFFFF) / 1024.0f;
        if (cyc == CYC_COPY) { dsdx /= 4; lrx += 1; lry += 1; }
        if (cyc == CYC_FILL) { lrx += 1; lry += 1; }
        Config c;
        compute_config(c, tile, true);
        c.zm = ortho_zm();
        use_config(c);
        float s1, t1;
        if (!flip) { s1 = s + dsdx * (lrx - ulx); t1 = t + dtdy * (lry - uly); }
        else { s1 = s + dsdx * (lry - uly); t1 = t + dtdy * (lrx - ulx); }
        float u0, v0, u1, v1;
        uv_of(c, s, t, u0, v0);
        uv_of(c, s1, t1, u1, v1);
        // Shade is not set for rectangles; 1 keeps SHADE-multiplied combiners visible.
        static const float shade[4] = { 1, 1, 1, 1 };
        BV b;
        if (cyc == CYC_COPY) { b.ca = 0xFFFFFFFFu; b.cb = 0; }
        else vertex_colour(shade, cyc == CYC_2 ? 2 : 1, false, b);
        float nx0 = 2 * ulx / cur_tw - 1, nx1 = 2 * lrx / cur_tw - 1, ny0 = 1 - 2 * uly / cur_th, ny1 = 1 - 2 * lry / cur_th;
        float px[4] = { nx0, nx1, nx1, nx0 }, py[4] = { ny0, ny0, ny1, ny1 };
        float pu[4], pv[4];
        if (!flip) { float a[4] = { u0, u1, u1, u0 }, bb[4] = { v0, v0, v1, v1 }; memcpy(pu, a, 16); memcpy(pv, bb, 16); }
        else { float a[4] = { u0, u0, u1, u1 }, bb[4] = { v0, v1, v1, v0 }; memcpy(pu, a, 16); memcpy(pv, bb, 16); }
        static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i = 0; i < 6; i++) {
            int k = idx[i];
            b.x = px[k]; b.y = py[k]; b.z = 0; b.u = pu[k]; b.v = pv[k];
            batch.push_back(b);
        }
    }

    void fill_rect(uint32_t w0, uint32_t w1) {
        int cyc = cycle_type();
        float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
        float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
        if (cyc >= CYC_COPY) { lrx += 1; lry += 1; }
        if (cimg_addr == zimg_addr && cyc == CYC_FILL) {
            flush();
            pending_zclear = true;
            return;
        }
        if (!select_target()) return;
        flush();
        have_cfg = false;
        int sc[4];
        scissor(sc);
        PassState s;
        base_state(s, ortho_zm(), sc);
        Vertex v[6];
        uint32_t col;
        if (cyc == CYC_FILL) {
            if (cimg_siz == 3) col = (fill >> 24) | ((fill >> 16) & 0xFF) << 8 | ((fill >> 8) & 0xFF) << 16 | 0xFF000000u;
            else col = rgba16((uint16_t)(fill >> 16)) | 0xFF000000u;
        } else {
            static const float shade[4] = { 1, 1, 1, 1 };
            BV b;
            vertex_colour(shade, cyc == CYC_2 ? 2 : 1, false, b);
            col = b.cb;
            uint32_t l = oml;
            int p = (l >> 30) & 3, m = (l >> 22) & 3, a = (l >> 26) & 3, bb = (l >> 18) & 3;
            if (cyc == CYC_2) { p = (l >> 28) & 3; a = (l >> 24) & 3; m = (l >> 20) & 3; bb = (l >> 16) & 3; }
            if ((l & FORCE_BL) && p == 0 && m == 1 && a == 0 && bb == 0) { s.blend = true; s.src = F_SRC_ALPHA; s.dst = F_INV_SRC_ALPHA; }
        }
        rect_vertices(ulx, uly, lrx, lry, 0, nullptr, v, col);
        cur_backend->draw(s, v, 6);
        self->stats.draws++;
        self->stats.passes++;
        if (cur_backend == gpu) { self->stats.gpu_passes++; self->stats.gpu_target = cur_target; }
    }

    // ---- Pass emission ----
    void emit(const PassState& s, const BV* b, int n, int colour_src) {
        Zone zone_(Z_EMIT);
        // colour_src: 0 = a (rgb) with alpha 1, 1 = b (rgb, alpha ba), 2 = a rgb with alpha a, 3 = white with alpha aa
        vout.resize(n);
        static_assert(offsetof(Vertex, r) == 20 && sizeof(Vertex) == 24, "Vertex layout");
        for (int i = 0; i < n; i++) {
            Vertex& o = vout[i];
            o.x = b[i].x; o.y = b[i].y; o.z = b[i].z; o.u = b[i].u; o.v = b[i].v;
            uint32_t c;
            switch (colour_src) {
            case 0: c = b[i].ca | 0xFF000000u; break;
            case 1: c = b[i].cb; break;
            case 2: c = b[i].ca; break;
            default: c = b[i].ca | 0x00FFFFFFu; break;
            }
            memcpy(&o.r, &c, 4);
        }
        if (trace) {
            // Pass detail: program, write mask, blend, depth, tint, colour source and the covered pixel box.
            float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
            for (int i = 0; i < n; i++) {
                float in4[4] = { vout[i].x, vout[i].y, vout[i].z, 1 }, o[4];
                for (int j = 0; j < 4; j++) o[j] = in4[0] * s.wvp[j] + in4[1] * s.wvp[4 + j] + in4[2] * s.wvp[8 + j] + s.wvp[12 + j];
                if (o[3] == 0) continue;
                float px = (o[0] / o[3] + 1) * 0.5f * cur_tw, py = (1 - o[1] / o[3]) * 0.5f * cur_th, pz = o[2] / o[3];
                bx0 = std::min(bx0, px); bx1 = std::max(bx1, px); by0 = std::min(by0, py); by1 = std::max(by1, py);
                z0 = std::min(z0, pz); z1 = std::max(z1, pz);
            }
            fprintf(trace, "  pass prog %d mask %X blend %d %d/%d depth %d w%d decal %d tint %.2f %.2f %.2f %.2f src %d box %.0f,%.0f-%.0f,%.0f z %.4f..%.4f sc %d,%d-%d,%d %s\n",
                (int)s.prog, s.mask, s.blend, s.src, s.dst, s.depth, s.depth_write, s.decal, s.tint[0], s.tint[1], s.tint[2], s.tint[3],
                colour_src, bx0, by0, bx1, by1, z0, z1, s.sx0, s.sy0, s.sx1, s.sy1, cur_backend == gpu ? "gpu" : "soft");
        }
        {
            Zone z(cur_backend == gpu ? Z_GPU : Z_SOFT);
            cur_backend->draw(s, vout.data(), n);
        }
        self->stats.path_passes[cur_path]++;
        self->stats.passes++;
        if (cur_backend == gpu) { self->stats.gpu_passes++; self->stats.gpu_target = cur_target; }
    }

    void flush() {
        Zone zone_(Z_FLUSH);
        if (batch.empty()) return;
        flush_uncounted();
    }

    void flush_uncounted() {
        if (!cur_backend) { batch.clear(); return; }
        self->stats.draws++;
        const Config& c = cfg;
        const int n = (int)batch.size();
        if (trace) {
            fprintf(trace, "draw tgt %06X mode %d tex %dx%d tile %d fmt %d/%d n %d cyc %d oml %08X omh %08X cc %06X %08X z %d%d%d ca %.2f %.2f %.2f %.2f cb %.2f %.2f %.2f %.2f\n",
                cur_target, c.mode, c.tex ? c.tex->w : 0, c.tex ? c.tex->h : 0, c.tile, tiles[c.tile].fmt, tiles[c.tile].siz, n, cycle_type(), oml, omh, cc0, cc1,
                c.ztest, c.zwrite, c.decal, ch8(batch[0].ca, 0), ch8(batch[0].ca, 1), ch8(batch[0].ca, 2), ch8(batch[0].ca, 3), ch8(batch[0].cb, 0), ch8(batch[0].cb, 1), ch8(batch[0].cb, 2), ch8(batch[0].cb, 3));
        }
        // Classify the batch on the quantised forms: texel rgb / alpha used at all, one rgb multiplier for every
        // vertex, any additive part; plus the average forms (0..1) for single-colour passes.
        uint32_t or_a = 0, or_b = 0, diff = 0;
        const uint32_t a0 = batch[0].ca;
        uint32_t sum_a[4] = {}, sum_b[4] = {};
        for (int i = 0; i < n; i++) {
            const BV& v = batch[i];
            or_a |= v.ca; or_b |= v.cb; diff |= v.ca ^ a0;
            sum_a[0] += v.ca & 0xFF; sum_a[1] += (v.ca >> 8) & 0xFF; sum_a[2] += (v.ca >> 16) & 0xFF; sum_a[3] += v.ca >> 24;
            sum_b[0] += v.cb & 0xFF; sum_b[1] += (v.cb >> 8) & 0xFF; sum_b[2] += (v.cb >> 16) & 0xFF; sum_b[3] += v.cb >> 24;
        }
        bool tex_rgb = (or_a & 0xFFFFFF) != 0, tex_a = (or_a >> 24) != 0, has_b = (or_b & 0xFFFFFF) != 0;
        bool uniform = (diff & 0xFFFFFF) == 0;
        float avg_a[4], avg_b[4];
        for (int ch = 0; ch < 4; ch++) { avg_a[ch] = sum_a[ch] / (255.0f * n); avg_b[ch] = sum_b[ch] / (255.0f * n); }
        PassState base;
        base_state(base, c.zm, c.sc);
        base.tex = c.tex;
        base.linear = c.linear;
        base.wrap_s = c.ws;
        base.wrap_t = c.wt;
        // Strictly nearer, like the RDP (and RT64) for fully covered pixels: a surface drawn again at exactly the
        // same depth loses. The hub's lava wall draws lava over the same triangles as the bricks before it.
        base.depth = c.ztest ? D_LESS : D_ALWAYS;
        base.depth_write = c.zwrite;
        base.decal = c.decal;
        bool depth = c.ztest || c.zwrite;
        PassState follow = base; // later passes: same pixels only, no depth write
        follow.depth = c.zwrite ? D_LEQUAL : base.depth;
        follow.depth_write = false;

        auto per_tri = [&](auto&& passes) {
            if (depth) passes(batch.data(), n);
            else { self->stats.split_draws++; for (int i = 0; i + 2 < n; i += 3) passes(batch.data() + i, 3); }
        };
        // Path statistics: 0 opaque shade, 1 opaque uniform tex, 2 opaque uniform tex + b, 3 opaque varying, 4 xlu/cutout
        // single pass, 5 cutout gouraud.
        auto path = [&](int p) { cur_path = p; self->stats.path_draws[p]++; };
        // Cutouts are coloured without depth writes (their passes blend by alpha); then the texels that are kept
        // write depth, so what is drawn later behind them (the hub's lava behind its brick wall) stays hidden.
        // `alpha` is the tint alpha the colour pass used: kept texels are those with texel alpha * alpha >= 0.5.
        auto cutout_depth = [&](const BV* b, int k, float alpha) {
            if (!c.zcut || !tex_a || !c.tex || !cur_backend->alpha_test()) return;
            PassState d = base;
            d.prog = Prog::TextureTint;
            d.mask = 0;
            d.depth_write = true;
            d.alpha_test = true;
            d.tint[3] = alpha;
            emit(d, b, k, 0);
        };
        // Multi-pass opaque draws compare with LEQUAL: their later passes find the base pass's own depth, and a
        // strict base test would let them through on pixels the base pass rejected at equal depth.
        const DepthFunc multi_depth = c.ztest ? D_LEQUAL : D_ALWAYS;

        if (c.mode == DM_OPAQUE) {
            if (!tex_rgb) {
                path(0);
                PassState s = base;
                emit(s, batch.data(), n, 1);
            } else if (uniform) {
                path(has_b ? 2 : 1);
                PassState s = base;
                s.prog = Prog::TextureTint;
                s.tint[0] = ch8(a0, 0); s.tint[1] = ch8(a0, 1); s.tint[2] = ch8(a0, 2); s.tint[3] = 1;
                if (s.tint[0] > 0.998f && s.tint[1] > 0.998f && s.tint[2] > 0.998f) s.prog = Prog::Texture;
                if (!has_b) emit(s, batch.data(), n, 0);
                else per_tri([&](const BV* b, int k) {
                    s.depth = multi_depth;
                    emit(s, b, k, 0);
                    PassState add = follow;
                    add.blend = true; add.src = F_ONE; add.dst = F_ONE;
                    emit(add, b, k, 1);
                });
            } else {
                path(3);
                per_tri([&](const BV* b, int k) {
                    PassState s = base;
                    s.depth = multi_depth;
                    emit(s, b, k, 0);
                    PassState t = follow;
                    t.prog = Prog::Texture;
                    t.blend = true; t.src = F_DST_COLOR; t.dst = F_ZERO;
                    emit(t, b, k, 0);
                    if (has_b) {
                        PassState add = follow;
                        add.blend = true; add.src = F_ONE; add.dst = F_ONE;
                        emit(add, b, k, 1);
                    }
                });
            }
        } else if (c.mode == DM_XLU || (c.mode == DM_CUTOUT && (uniform || !tex_rgb))) {
            path(4);
            PassState s = base;
            s.blend = true;
            s.src = c.mode == DM_XLU ? c.bsrc : F_SRC_ALPHA;
            s.dst = c.mode == DM_XLU ? c.bdst : F_INV_SRC_ALPHA;
            if (!tex_rgb && !tex_a) {
                emit(s, batch.data(), n, 1);
            } else {
                s.prog = Prog::TextureTint;
                auto set_tint = [&](const float* fa, const float* fb) {
                    if (tex_rgb) for (int ch = 0; ch < 3; ch++) s.tint[ch] = clamp01(fa[ch] + fb[ch]);
                    else for (int ch = 0; ch < 3; ch++) s.tint[ch] = clamp01(fb[ch]);
                    s.tint[3] = clamp01(tex_a ? fa[3] + fb[3] : fb[3]);
                };
                // Flat-coloured triangles get one tint per run of equal colour: the average would blend, say, the
                // highlighted item of a menu (one draw, the colour from PRIM changing per item) into the others.
                bool flat = true;
                int runs = 1;
                for (int i = 0; i + 2 < n && flat; i += 3) {
                    const BV* t = &batch[i];
                    flat = t[1].ca == t[0].ca && t[2].ca == t[0].ca && t[1].cb == t[0].cb && t[2].cb == t[0].cb;
                    if (i && (t[0].ca != t[-3].ca || t[0].cb != t[-3].cb)) runs++;
                }
                if (flat && runs > 1 && n % 3 == 0) {
                    for (int i = 0; i < n;) {
                        int j = i + 3;
                        while (j < n && batch[j].ca == batch[i].ca && batch[j].cb == batch[i].cb) j += 3;
                        float fa[4], fb[4];
                        for (int ch = 0; ch < 4; ch++) { fa[ch] = ch8(batch[i].ca, ch); fb[ch] = ch8(batch[i].cb, ch); }
                        set_tint(fa, fb);
                        emit(s, batch.data() + i, j - i, 0);
                        if (c.mode == DM_CUTOUT) cutout_depth(batch.data() + i, j - i, s.tint[3]);
                        i = j;
                    }
                } else {
                    set_tint(avg_a, avg_b);
                    emit(s, batch.data(), n, 0);
                    if (c.mode == DM_CUTOUT) cutout_depth(batch.data(), n, s.tint[3]);
                }
            }
        } else { // cutout with Gouraud-shaded texture
            path(5);
            Wrap ws, wt;
            int w, h;
            const TexImage* pm = texture(c.tile, true, ws, wt, w, h);
            per_tri([&](const BV* b, int k) {
                PassState m = follow;               // 1: texel alpha into the target's alpha
                m.depth = base.depth;
                m.prog = Prog::TextureTint;
                m.mask = M_A;
                emit(m, b, k, 3);
                PassState col = m;                  // 2: shade where covered
                col.prog = Prog::Color;
                col.mask = M_RGB;
                col.blend = true; col.src = F_DST_ALPHA; col.dst = F_INV_DST_ALPHA;
                emit(col, b, k, 0);
                PassState t = col;                  // 3: times the texel where covered
                t.prog = Prog::Texture;
                t.tex = pm;
                t.src = F_DST_COLOR; t.dst = F_INV_SRC_ALPHA;
                emit(t, b, k, 0);
                if (has_b) {
                    PassState add = col;
                    add.src = F_DST_ALPHA; add.dst = F_ONE;
                    emit(add, b, k, 1);
                }
            });
            cutout_depth(batch.data(), n, 1.0f);   // the mask pass above tests texel alpha alone
        }
        batch.clear();
    }

    // ---- Display list ----
    // Waits until the command at a differs from (w0, w1) and reads the same twice in a row; false after 250 ms.
    bool wait_for_rewrite(uint32_t a, uint32_t w0, uint32_t w1) {
        auto start = std::chrono::steady_clock::now();
        uint32_t p0 = w0, p1 = w1;
        bool changed = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::microseconds(changed ? 20 : 100));
            uint32_t n0 = rd32(a), n1 = rd32(a + 4);
            if (n0 != w0 || n1 != w1) {
                if (changed && n0 == p0 && n1 == p1) { self->stats.list_waits++; return true; }
                changed = true;
            }
            p0 = n0; p1 = n1;
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(250)) {
                self->stats.list_wait_timeouts++;
                return false;
            }
        }
    }

    void run(uint32_t pc) {
        Zone zone_(Z_PARSE);
        uint32_t stack[18];
        int sp = 0;
        for (auto& s : seg) s = 0;
        zmodes.clear();
        mvp_dirty = true;
        have_cfg = false;
        cfg_dirty = true;
        tri_cfg_valid = false;
        cfg_from_tri = false;
        cur_target = 0;
        sel_addr = 0;
        cur_backend = nullptr;
        frame++;
        // Debug: n64trace.go in the working directory traces every draw of the next display list to n64trace.txt.
        if (trace) { fclose(trace); trace = nullptr; }
        if ((frame % 30) == 0) {
            std::string go = self->debug_dir + "n64trace.go";
            if (remove(go.c_str()) == 0) trace = fopen((self->debug_dir + "n64trace.txt").c_str(), "w");
        }
        pc &= ADDR_MASK;
        const uint32_t root = pc;
        br_count = 0;
        for (int budget = 0; budget < 400000; budget++) {
            // The game may be rewriting this command (see wait_for_rewrite): w0 is read again after w1, so a fetch
            // between its two stores (old w0, new w1) is retried rather than run as half a command.
            const volatile uint32_t* cmd = (const volatile uint32_t*)(rdram + (pc & ADDR_MASK & ~3u));
            uint32_t w0 = cmd[0], w1 = cmd[1];
            for (uint32_t again; (again = cmd[0]) != w0;) { w0 = again; w1 = cmd[1]; }
            pc += 8;
            switch (w0 >> 24) {
            case 0xD7: case 0xD9: case 0xE2: case 0xE3: case 0xED: case 0xEF: case 0xFC: case 0xFF:
                cfg_dirty = true;
                if (++cc_ver == 0) cc_ver = 1;
                break;
            case 0xEA: case 0xEB: case 0xF8: case 0xFA: case 0xFB:
                if (++cc_ver == 0) cc_ver = 1;
                break;
            default: break;
            }
            switch (w0 >> 24) {
            case 0x00: break;
            case 0x01: { // G_VTX
                int n = (w0 >> 12) & 0xFF, end = (w0 >> 1) & 0x7F;
                // A malformed load (n > end) would write before vbuf and wreck the members in front of it.
                if (n > end) {
                    auto& st = self->stats;
                    st.bad_cmds++;
                    st.bad_pc = pc - 8; st.bad_w0 = w0; st.bad_w1 = w1; st.bad_root = root; st.bad_depth = sp;
                    for (int i = 0; i < 4; i++) {
                        const auto& b = br_ring[(br_count - 1 - i) & 7];
                        st.bad_branch[i][0] = i < br_count ? b[0] : 0;
                        st.bad_branch[i][1] = i < br_count ? b[1] : 0;
                        st.bad_branch[i][2] = i < br_count ? b[2] : 0;
                    }
                    break;
                }
                load_vertices(segaddr(w1), n, end - n);
                break;
            }
            case 0x03: { // G_CULLDL
                int a = (w0 & 0xFFFF) / 2, b = (w1 & 0xFFFF) / 2;
                uint32_t all = 0xF;
                for (int i = a; i <= b && i < 64; i++) {
                    const RV& v = vbuf[i];
                    uint32_t f = (v.x < -v.w ? 1 : 0) | (v.x > v.w ? 2 : 0) | (v.y < -v.w ? 4 : 0) | (v.y > v.w ? 8 : 0);
                    all &= f;
                }
                if (all) { if (sp == 0) goto done; pc = stack[--sp]; }
                break;
            }
            case 0x05: triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2); break;
            case 0x06:
            case 0x07:
                triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                triangle(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                break;
            case 0xD7: // G_TEXTURE
                tex_scale_s = ((w1 >> 16) & 0xFFFF) / 65536.0f;
                tex_scale_t = (w1 & 0xFFFF) / 65536.0f;
                if ((w1 >> 16) == 0xFFFF) tex_scale_s = 1;
                if ((w1 & 0xFFFF) == 0xFFFF) tex_scale_t = 1;
                tex_tile = (w0 >> 8) & 7;
                tex_on = ((w0 >> 1) & 0x7F) != 0;
                break;
            case 0xD8: { // G_POPMTX
                int n = w1 / 64;
                while (n-- > 0 && mv_sp > 0) mv = mv_stack[--mv_sp];
                mvp_dirty = true;
                break;
            }
            case 0xD9: geom = (geom & (w0 & 0xFFFFFF)) | w1; break;
            case 0xDA: { // G_MTX
                Zone zone_(Z_MTX);
                uint32_t p = (w0 & 0xFF) ^ 1;
                Mtx m;
                load_mtx(segaddr(w1), m);
                if (p & 4) {
                    if (p & 2) proj = m; else mul(m, proj, proj);
                } else {
                    if ((p & 1) && mv_sp < 32) mv_stack[mv_sp++] = mv;
                    if (p & 2) mv = m; else mul(m, mv, mv);
                }
                mvp_dirty = true;
                break;
            }
            case 0xDB: { // G_MOVEWORD
                int idx = (w0 >> 16) & 0xFF, off = w0 & 0xFFFF;
                if (idx == 2) num_lights = std::min<int>(w1 / 24, 7);
                else if (idx == 6) seg[(off / 4) & 0xF] = w1 & 0xFFFFFF;
                else if (idx == 8) { fog_mul = (int16_t)(w1 >> 16); fog_off = (int16_t)(w1 & 0xFFFF); }
                else if (idx == 0x0A && (off & 7) == 0) {
                    int l = off / 0x18;
                    if (l < 8) { light_col[l][0] = (w1 >> 24) / 255.0f; light_col[l][1] = ((w1 >> 16) & 0xFF) / 255.0f; light_col[l][2] = ((w1 >> 8) & 0xFF) / 255.0f; }
                }
                break;
            }
            case 0xDC: { // G_MOVEMEM
                int idx = w0 & 0xFF, off = ((w0 >> 8) & 0xFF) * 8;
                uint32_t a = segaddr(w1);
                if (idx == 8) {
                    for (int i = 0; i < 3; i++) {
                        vp_scale[i] = (int16_t)rd16(a + i * 2) / 4.0f;
                        vp_trans[i] = (int16_t)rd16(a + 8 + i * 2) / 4.0f;
                    }
                } else if (idx == 10 && off >= 0x30) {
                    int l = (off - 0x30) / 0x18;
                    if (l < 8) {
                        for (int c = 0; c < 3; c++) {
                            light_col[l][c] = rd8(a + c) / 255.0f;
                            light_dir[l][c] = (int8_t)rd8(a + 8 + c);
                        }
                    }
                }
                break;
            }
            case 0xDE: // G_DL
                // A branch to itself is the game holding the RSP while it finishes the list: it rewrites this command
                // later (with the next part, or an end). Wait for the rewrite instead of spinning through the command
                // budget (the list was then cut after its clear: a black frame), and re-read it once both words are
                // stable (a read between the game's two stores gave half a command, a branch to address 0).
                if (segaddr(w1) == pc - 8) {
                    if (!wait_for_rewrite(pc - 8, w0, w1)) goto done;
                    pc -= 8;
                    break;
                }
                if (((w0 >> 16) & 0xFF) == 0 && sp < 18) stack[sp++] = pc;
                br_ring[br_count++ & 7] = { pc - 8, w1, segaddr(w1) };
                pc = segaddr(w1);
                break;
            case 0xDF: // G_ENDDL
                if (sp == 0) goto done;
                pc = stack[--sp];
                break;
            case 0xE1: case 0xF1: break; // RDPHALF outside a texrect
            case 0xE2: case 0xE3: { // SETOTHERMODE_L / _H
                uint32_t len = (w0 & 0xFF) + 1, shift = 32 - ((w0 >> 8) & 0xFF) - len;
                uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
                uint32_t& m = (w0 >> 24) == 0xE2 ? oml : omh;
                m = (m & ~mask) | (w1 & mask);
                break;
            }
            case 0xEF: omh = w0 & 0xFFFFFF; oml = w1; break;
            case 0xE4: case 0xE5: { // TEXRECT: the next two commands carry s/t and ds/dt
                uint32_t h1 = rd32(pc + 4), h2 = rd32(pc + 12);
                pc += 16;
                tex_rect(w0, w1, h1, h2, (w0 >> 24) == 0xE5);
                break;
            }
            case 0xED: // SETSCISSOR
                sc_ulx = ((w0 >> 12) & 0xFFF) / 4.0f; sc_uly = (w0 & 0xFFF) / 4.0f;
                sc_lrx = ((w1 >> 12) & 0xFFF) / 4.0f; sc_lry = (w1 & 0xFFF) / 4.0f;
                break;
            case 0xEA: // SETKEYGB
                key_center[1] = (w1 >> 24) / 255.0f; key_scale[1] = ((w1 >> 16) & 0xFF) / 255.0f;
                key_center[2] = ((w1 >> 8) & 0xFF) / 255.0f; key_scale[2] = (w1 & 0xFF) / 255.0f;
                break;
            case 0xEB: key_center[0] = ((w1 >> 8) & 0xFF) / 255.0f; key_scale[0] = (w1 & 0xFF) / 255.0f; break;
            case 0xF0: load_tlut((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF); break;
            case 0xF2: { // SETTILESIZE
                Tile t = tiles[(w1 >> 24) & 7];
                t.uls = (w0 >> 12) & 0xFFF; t.ult = w0 & 0xFFF; t.lrs = (w1 >> 12) & 0xFFF; t.lrt = w1 & 0xFFF;
                set_tile((w1 >> 24) & 7, t);
                break;
            }
            case 0xF3: load_block((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF); break;
            case 0xF4: load_tile((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF); break;
            case 0xF5: { // SETTILE
                Tile t = tiles[(w1 >> 24) & 7];
                t.fmt = (w0 >> 21) & 7; t.siz = (w0 >> 19) & 3; t.line = (w0 >> 9) & 0x1FF; t.tmem = w0 & 0x1FF;
                t.palette = (w1 >> 20) & 0xF; t.cmt = (w1 >> 18) & 3; t.maskt = (w1 >> 14) & 0xF; t.shiftt = (w1 >> 10) & 0xF;
                t.cms = (w1 >> 8) & 3; t.masks = (w1 >> 4) & 0xF; t.shifts = w1 & 0xF;
                set_tile((w1 >> 24) & 7, t);
                break;
            }
            case 0xF6: fill_rect(w0, w1); break;
            case 0xF7: fill = w1; break;
            case 0xF8: color(w1, fogc); break;
            case 0xF9: color(w1, blendc); break;
            case 0xFA: color(w1, prim); prim_lod = (w0 & 0xFF) / 255.0f; break;
            case 0xFB: color(w1, env); break;
            case 0xFC: cc0 = w0 & 0xFFFFFF; cc1 = w1; break;
            case 0xFD: timg_siz = (w0 >> 19) & 3; timg_width = (w0 & 0xFFF) + 1; timg_addr = segaddr(w1); break;
            case 0xFE: zimg_addr = segaddr(w1); break;
            case 0xFF: flush(); cimg_siz = (w0 >> 19) & 3; cimg_width = (w0 & 0xFFF) + 1; cimg_addr = segaddr(w1); break;
            default: break;
            }
        }
    done:
        flush();
        gpu->finish();
        evict_textures();
    }
};

// ---------------------------------------------------------------------------------------------------------------

Renderer::Renderer(uint8_t* rdram, Backend* gpu, bool offscreen_to_rdram) : impl(std::make_unique<Impl>(this, rdram, gpu, offscreen_to_rdram)) {}
Renderer::~Renderer() = default;

void Renderer::run(uint32_t dl) { impl->run(dl); }

SoftBackend* Renderer::offscreen() { return &impl->soft; }

void Renderer::note_vi_origin(uint32_t origin) {
    origin &= ADDR_MASK;
    // The colour image the origin falls in: the closest one drawn at or below it (the VI origin can point a few
    // rows past the image start).
    uint32_t best = 0;
    for (const auto& [addr, h] : impl->target_height)
        if (addr <= origin && origin < addr + 2 * 640 * 8 && addr > best) best = addr;
    if (best) impl->displayed.insert(best);
}

uint32_t Renderer::gpu_target_for(uint32_t origin) const {
    origin &= ADDR_MASK;
    uint32_t best = 0;
    for (uint32_t addr : impl->gpu_drawn)
        if (origin >= addr && origin < addr + 2 * 640 * 8 && addr > best) best = addr;
    return best;
}

void Renderer::decode_rdram(uint32_t origin, int w, int h, std::vector<uint32_t>& out) const {
    out.resize((size_t)w * h);
    origin &= ADDR_MASK;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) out[(size_t)y * w + x] = Impl::rgba16(impl->rd16(origin + (y * w + x) * 2)) | 0xFF000000u;
}

// ---------------------------------------------------------------------------------------------------------------
// Software backend

void SoftBackend::begin_target(uint32_t addr, int width, int height) {
    cur_addr = addr;
    cur_w = width;
    cur_h = height;
    cur_usage = &usage[addr];
    cur_usage->addr = addr; cur_usage->w = width; cur_usage->h = height;
    if (!to_rdram) {
        Target& t = targets[addr];
        if (t.w != width || t.h != height) { t.w = width; t.h = height; t.px.assign((size_t)width * height, 0xFF000000u); }
        cur = &t;
    } else {
        targets[addr];
    }
    if (depth_w < width || depth_h < height) {
        depth_w = std::max(depth_w, width);
        depth_h = std::max(depth_h, height);
        depth.assign((size_t)depth_w * depth_h, 1.0f);
        scratch_alpha.assign((size_t)depth_w * depth_h, 0);
    }
}

std::vector<SoftBackend::Usage> SoftBackend::take_usage(size_t n) {
    std::vector<Usage> out;
    for (auto& [a, u] : usage) if (u.draws) { out.push_back(u); u.draws = u.pixels = 0; }
    std::sort(out.begin(), out.end(), [](const Usage& x, const Usage& y) { return x.pixels > y.pixels; });
    if (out.size() > n) out.resize(n);
    return out;
}

const std::vector<uint32_t>* SoftBackend::image(uint32_t addr, int* w, int* h) const {
    auto it = targets.find(addr);
    if (it == targets.end() || it->second.px.empty()) return nullptr;
    *w = it->second.w;
    *h = it->second.h;
    return &it->second.px;
}

uint32_t SoftBackend::read_px(int x, int y) {
    if (!to_rdram) return cur->px[(size_t)y * cur_w + x];
    uint32_t a = (cur_addr + (uint32_t)(y * cur_w + x) * 2) & ADDR_MASK;
    uint16_t c = *(uint16_t*)(rdram + (a ^ 2));
    uint32_t r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
    return (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16 | ((c & 1) ? 0xFF000000u : 0);
}

void SoftBackend::write_px(int x, int y, uint32_t c) {
    if (!to_rdram) { cur->px[(size_t)y * cur_w + x] = c; return; }
    uint32_t a = (cur_addr + (uint32_t)(y * cur_w + x) * 2) & ADDR_MASK;
    uint16_t v = (uint16_t)(((c & 0xFF) >> 3) << 11 | (((c >> 8) & 0xFF) >> 3) << 6 | (((c >> 16) & 0xFF) >> 3) << 1 | ((c >> 24) >= 128 ? 1 : 0));
    *(uint16_t*)(rdram + (a ^ 2)) = v;
}

namespace {
    // In-range coordinates (nearly all) and power-of-two sizes avoid integer division (software on the Vita).
    inline int wrap(int c, int n, Wrap m) {
        if ((unsigned)c < (unsigned)n) return c;
        if (m == W_CLAMP) return c < 0 ? 0 : n - 1;
        if (m == W_REPEAT) return (n & (n - 1)) == 0 ? (c & (n - 1)) : ((c % n) + n) % n;
        int p = (n & (n - 1)) == 0 ? (c & (2 * n - 1)) : ((c % (2 * n)) + 2 * n) % (2 * n);
        return p < n ? p : 2 * n - 1 - p;
    }

    inline int ifloor(float f) { int i = (int)f; return i - (f < (float)i); }

    inline uint32_t fetch(const TexImage* t, int x, int y, Wrap ws, Wrap wt) {
        return t->px[(size_t)wrap(y, t->h, wt) * t->w + wrap(x, t->w, ws)];
    }

    // x, y in texels (u * width, v * height). Returns RGBA8.
    inline uint32_t sample(const PassState& s, const TexImage* t, float x, float y) {
        if (!s.linear) return fetch(t, ifloor(x), ifloor(y), s.wrap_s, s.wrap_t);
        x -= 0.5f; y -= 0.5f;
        int x0 = ifloor(x), y0 = ifloor(y);
        uint32_t fx = (uint32_t)((x - x0) * 256), fy = (uint32_t)((y - y0) * 256);
        uint32_t a = fetch(t, x0, y0, s.wrap_s, s.wrap_t), b = fetch(t, x0 + 1, y0, s.wrap_s, s.wrap_t);
        uint32_t c = fetch(t, x0, y0 + 1, s.wrap_s, s.wrap_t), d = fetch(t, x0 + 1, y0 + 1, s.wrap_s, s.wrap_t);
        // Two channels at a time (bytes 0 and 2, then 1 and 3), 8-bit weights.
        auto lerp2 = [](uint32_t p, uint32_t q, uint32_t f) {
            uint32_t lo = ((p & 0x00FF00FF) * (256 - f) + (q & 0x00FF00FF) * f) >> 8 & 0x00FF00FF;
            uint32_t hi = (((p >> 8) & 0x00FF00FF) * (256 - f) + ((q >> 8) & 0x00FF00FF) * f) & 0xFF00FF00;
            return lo | hi;
        };
        return lerp2(lerp2(a, b, fx), lerp2(c, d, fx), fy);
    }

    // x / 255 rounded, for x up to 2 * 255 * 255.
    inline uint32_t div255(uint32_t x) { x += 128; return (x + (x >> 8)) >> 8; }
}

// cv: clip-space vertices after wvp, 10 floats each: x y z w u v r g b a.
void SoftBackend::draw(const PassState& s, const Vertex* v, int count) {
    if (cur_w <= 0 || cur_h <= 0 || (!to_rdram && !cur)) return;
    if (cur_usage) cur_usage->draws++;
    for (int t = 0; t + 2 < count; t += 3) {
        float in[3][10];
        for (int k = 0; k < 3; k++) {
            const Vertex& p = v[t + k];
            float i4[4] = { p.x, p.y, p.z, 1 };
            for (int j = 0; j < 4; j++) in[k][j] = i4[0] * s.wvp[j] + i4[1] * s.wvp[4 + j] + i4[2] * s.wvp[8 + j] + s.wvp[12 + j];
            in[k][4] = p.u; in[k][5] = p.v;
            in[k][6] = p.r / 255.0f; in[k][7] = p.g / 255.0f; in[k][8] = p.b / 255.0f; in[k][9] = p.a / 255.0f;
        }
        // Clip against z >= 0 and z <= w (Sutherland-Hodgman), then fan.
        float poly[9][10], tmp[9][10];
        int n = 3;
        memcpy(poly, in, sizeof(in));
        for (int plane = 0; plane < 2 && n > 0; plane++) {
            int m = 0;
            for (int i = 0; i < n; i++) {
                const float* a = poly[i];
                const float* b = poly[(i + 1) % n];
                float da = plane == 0 ? a[2] : a[3] - a[2], db = plane == 0 ? b[2] : b[3] - b[2];
                if (da >= 0) memcpy(tmp[m++], a, sizeof(float) * 10);
                if ((da >= 0) != (db >= 0)) {
                    float f = da / (da - db);
                    for (int j = 0; j < 10; j++) tmp[m][j] = a[j] + (b[j] - a[j]) * f;
                    m++;
                }
            }
            n = m;
            memcpy(poly, tmp, sizeof(float) * 10 * m);
        }
        for (int i = 1; i + 1 < n; i++) {
            float tri[3][10];
            memcpy(tri[0], poly[0], sizeof(tri[0]));
            memcpy(tri[1], poly[i], sizeof(tri[0]));
            memcpy(tri[2], poly[i + 1], sizeof(tri[0]));
            raster(s, tri);
        }
    }
}

void SoftBackend::raster(const PassState& s, const float (*cv)[10]) {
    float sx[3], sy[3], sz[3], iw[3];
    for (int k = 0; k < 3; k++) {
        float w = cv[k][3];
        if (w <= 1e-7f) return;
        iw[k] = 1 / w;
        sx[k] = (cv[k][0] * iw[k] + 1) * 0.5f * cur_w;
        sy[k] = (1 - cv[k][1] * iw[k]) * 0.5f * cur_h;
        sz[k] = cv[k][2] * iw[k];
    }
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
    if (fabsf(area) < 1e-9f) return;
    int x0 = std::max({ s.sx0, 0, (int)floorf(std::min({ sx[0], sx[1], sx[2] })) });
    int x1 = std::min({ s.sx1, cur_w, (int)ceilf(std::max({ sx[0], sx[1], sx[2] })) });
    int y0 = std::max({ s.sy0, 0, (int)floorf(std::min({ sy[0], sy[1], sy[2] })) });
    int y1 = std::min({ s.sy1, cur_h, (int)ceilf(std::max({ sy[0], sy[1], sy[2] })) });
    bool use_depth = s.depth != D_ALWAYS || s.depth_write;
    uint32_t covered = 0;
    struct CountCovered { uint32_t& c; Usage* u; ~CountCovered() { if (u) u->pixels += c; } } count_covered{ covered, cur_usage };
    // Barycentrics are linear in the pixel position: step them, and every attribute, along the row. Attributes
    // (f = f2 + b0 (f0 - f2) + b1 (f1 - f2)): z, 1/w, u * width, v * height, colour * 255; all but z divided by w
    // unless w is equal at the three vertices (2D draws such as the HUD), where they interpolate linearly.
    const float inv_area = 1 / area;
    const float db0dx = (sy[1] - sy[2]) * inv_area, db1dx = (sy[2] - sy[0]) * inv_area;
    const bool affine = fabsf(iw[0] - iw[1]) <= 1e-6f * fabsf(iw[0]) && fabsf(iw[0] - iw[2]) <= 1e-6f * fabsf(iw[0]);
    const TexImage* tex = s.prog == Prog::Color ? nullptr : s.tex;
    const float scale[6] = { tex ? (float)tex->w : 1, tex ? (float)tex->h : 1, 255, 255, 255, 255 };
    float fv[3][8];
    for (int k = 0; k < 3; k++) {
        float q = affine ? 1 : iw[k];
        fv[k][0] = sz[k];
        fv[k][1] = q;
        for (int j = 0; j < 6; j++) fv[k][2 + j] = cv[k][4 + j] * scale[j] * q;
    }
    float e0[8], e1[8], dfdx[8];
    for (int a = 0; a < 8; a++) {
        e0[a] = fv[0][a] - fv[2][a];
        e1[a] = fv[1][a] - fv[2][a];
        dfdx[a] = db0dx * e0[a] + db1dx * e1[a];
    }
    // Colour maths in bytes. tint in 1/256 steps; blend factors pick from a table of candidate values.
    uint32_t tint[4];
    for (int i = 0; i < 4; i++) tint[i] = (uint32_t)std::max(0.0f, std::min(s.tint[i], 64.0f) * 256 + 0.5f);
    const bool tinted = tex && s.prog == Prog::TextureTint;
    const bool need_dst = s.blend || s.mask != M_ALL;
    const bool force_alpha = to_rdram && s.mask == M_ALL; // RGBA16 coverage bit, see below
    // RGBA16 images: a cutout's alpha-only mask pass goes to a scratch plane instead of the coverage bit (the RDP
    // leaves pixels that fail the alpha test untouched), and the passes that blend with destination alpha read it.
    const bool mask_pass = to_rdram && s.mask == M_A;
    auto is_dst_alpha = [](Factor f) { return f == F_DST_ALPHA || f == F_INV_DST_ALPHA; };
    const bool reads_mask = to_rdram && s.blend && (is_dst_alpha(s.src) || is_dst_alpha(s.dst));
    for (int y = y0; y < y1; y++) {
        float py = y + 0.5f, px0 = x0 + 0.5f;
        float rb0 = ((sx[1] - px0) * (sy[2] - py) - (sy[1] - py) * (sx[2] - px0)) * inv_area;
        float rb1 = ((sx[2] - px0) * (sy[0] - py) - (sy[2] - py) * (sx[0] - px0)) * inv_area;
        // The covered span of this row from the three edge functions (b(x) = r + d * (x - x0) >= 0), widened by a
        // pixel each side; the per-pixel test below stays exact.
        int xs = x0, xe = x1;
        {
            const float r[3] = { rb0, rb1, 1 - rb0 - rb1 }, d[3] = { db0dx, db1dx, -db0dx - db1dx };
            float lo = 0, hi = (float)(x1 - x0);
            for (int k = 0; k < 3; k++) {
                if (d[k] > 0) lo = std::max(lo, -r[k] / d[k]);
                else if (d[k] < 0) hi = std::min(hi, -r[k] / d[k]);
                else if (r[k] < 0) hi = -1;
            }
            if (hi < lo - 2) continue;
            xs = std::max(x0, x0 + (int)lo - 1);
            xe = std::min(x1, x0 + (int)hi + 2);
        }
        rb0 += db0dx * (xs - x0);
        rb1 += db1dx * (xs - x0);
        float f[8];
        for (int a = 0; a < 8; a++) f[a] = fv[2][a] + rb0 * e0[a] + rb1 * e1[a];
        for (int x = xs; x < xe; x++, rb0 += db0dx, rb1 += db1dx) {
            float b0 = rb0, b1 = rb1;
            float b2 = 1 - b0 - b1;
            if (b0 < 0 || b1 < 0 || b2 < 0) { for (int a = 0; a < 8; a++) f[a] += dfdx[a]; continue; }
            covered++;
            float z = f[0];
            float iq = affine ? 1.0f : 1.0f / f[1];
            float u = f[2] * iq, v = f[3] * iq;
            float cr = f[4] * iq, cg = f[5] * iq, cb = f[6] * iq, ca = f[7] * iq;
            for (int a = 0; a < 8; a++) f[a] += dfdx[a];
            float* dz = &depth[(size_t)y * depth_w + x];
            if (use_depth) {
                float zz = s.decal ? z - 1e-4f : z;
                if (s.depth == D_LESS && !(zz < *dz)) continue;
                if (s.depth == D_LEQUAL && !(zz <= *dz + 1e-6f)) continue;
            }
            if (s.alpha_test && tex && (sample(s, tex, u, v) >> 24) * tint[3] < 128u * 256u) continue;
            if (s.depth_write) *dz = z;
            if (s.mask == 0) continue;
            uint32_t src[4];
            if (!tex) {
                auto c8 = [](float c) { return c <= 0 ? 0u : c >= 255 ? 255u : (uint32_t)(c + 0.5f); };
                src[0] = c8(cr); src[1] = c8(cg); src[2] = c8(cb); src[3] = c8(ca);
            } else {
                uint32_t t = sample(s, tex, u, v);
                for (int i = 0; i < 4; i++) {
                    src[i] = (t >> (i * 8)) & 0xFF;
                    if (tinted) src[i] = std::min<uint32_t>((src[i] * tint[i] + 128) >> 8, 255);
                }
            }
            uint32_t dst[4] = { 0, 0, 0, 0 };
            if (need_dst) {
                uint32_t dc = read_px(x, y);
                for (int i = 0; i < 4; i++) dst[i] = (dc >> (i * 8)) & 0xFF;
            }
            const uint32_t cvg = dst[3];
            uint8_t* sa = to_rdram ? &scratch_alpha[(size_t)y * depth_w + x] : nullptr;
            if (mask_pass || reads_mask) dst[3] = *sa;
            uint32_t out = 0;
            for (int i = 0; i < 4; i++) {
                uint32_t o = src[i];
                if (s.blend) {
                    // Same order as Factor.
                    const uint32_t cand[10] = { 0, 255, src[i], 255 - src[i], src[3], 255 - src[3],
                                                dst[i], 255 - dst[i], dst[3], 255 - dst[3] };
                    o = std::min<uint32_t>(div255(src[i] * cand[s.src] + dst[i] * cand[s.dst]), 255);
                }
                if (!(s.mask & (1 << i))) o = dst[i];
                out |= o << (i * 8);
            }
            // RGBA16 images keep coverage in the alpha bit: a full draw sets it, a cutout's colour pass sets it where
            // the mask passed, other passes keep it.
            if (mask_pass) { *sa = (uint8_t)(out >> 24); continue; }
            if (to_rdram && s.mask != M_ALL && !(s.mask & M_A)) {
                uint32_t a = reads_mask && *sa >= 128 ? 255 : cvg;
                out = (out & 0x00FFFFFFu) | a << 24;
            }
            if (force_alpha) out |= 0xFF000000u;
            write_px(x, y, out);
        }
    }
}

} // namespace n64gfx
