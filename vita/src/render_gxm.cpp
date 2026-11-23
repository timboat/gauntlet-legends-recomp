// Vita renderer: gfx/n64gfx (F3DEX2 on the CPU) drawing with raw GXM and vita2d's precompiled shaders (colour,
// texture, texture x tint), so no runtime shader compiler (libshacccg) is needed. Each displayed N64 framebuffer
// is an offscreen render target (2x the N64 size, with its own depth buffer); update_screen shows the one the VI
// points at, scaled to 4:3.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <vita2d.h>
#include <vitashark.h>

#include "n64gfx.h"
#include "vita_log.h"
#include "vita_render.h"

extern "C" {
    extern const SceGxmProgram color_v_gxp_start, color_f_gxp_start;
    extern const SceGxmProgram texture_v_gxp_start, texture_f_gxp_start, texture_tint_f_gxp_start;
}

extern std::atomic<uint32_t> stat_dl_count;

// For perf.txt: where the graphics thread is (1 display list, 2 present, 3 swap, 4 GPU wait, 0 idle) and totals.
std::atomic<int> render_stage{ 0 };
static const n64gfx::Renderer::Stats* render_stats = nullptr;
static std::atomic<uint32_t> tex_pool_kb{ 0 };
// Graphics-thread time per perf window: running display lists, presenting; presents made and skipped as unchanged.
static std::atomic<uint64_t> us_run{ 0 }, us_present{ 0 };
static std::atomic<uint32_t> presents{ 0 }, presents_same{ 0 };
// Present paths per perf window: a GPU target, an RDRAM image (CPU-drawn frame), or blank (no usable VI mode).
static std::atomic<uint32_t> presents_gpu{ 0 }, presents_rdram{ 0 }, presents_blank{ 0 };
// Sampling profiler: a thread reads n64gfx::prof_zone every 250 us; perf.txt gets the share of samples per zone.
static std::atomic<uint32_t> prof_samples[n64gfx::Z_COUNT];
// Offscreen buffers drawn on the CPU (largest first), refreshed by the graphics thread every 150 lists.
static std::mutex soft_usage_lock;
static char soft_usage_text[160];
static int prof_thread(SceSize, void*) {
    for (;;) {
        sceKernelDelayThread(250);
        prof_samples[n64gfx::prof_zone]++;
    }
    return 0;
}

void vita_render_stats(char* buf, size_t n) {
    if (!render_stats) { snprintf(buf, n, "stage %d", render_stage.load()); return; }
    const auto& s = *render_stats;
    int k = snprintf(buf, n, "stage %d tris %u draws %u passes %u decodes %u textures %u pool %u KB bad cmds %u list waits %u (gave up %u)", render_stage.load(),
        s.tris, s.draws, s.passes, s.tex_decodes, s.textures, tex_pool_kb.load(), s.bad_cmds, s.list_waits, s.list_wait_timeouts);
    if (k > 0 && (size_t)k < n)
        snprintf(buf + k, n - k, "\n  gfx thread: dl %llu ms, present %llu ms, presents %u (gpu %u, rdram %u, blank %u), unchanged skipped %u",
            (unsigned long long)(us_run.exchange(0) / 1000),
            (unsigned long long)(us_present.exchange(0) / 1000), presents.exchange(0), presents_gpu.exchange(0),
            presents_rdram.exchange(0), presents_blank.exchange(0), presents_same.exchange(0));
    uint32_t counts[n64gfx::Z_COUNT], total = 0;
    for (int z = 0; z < n64gfx::Z_COUNT; z++) total += counts[z] = prof_samples[z].exchange(0);
    size_t used = strlen(buf);
    {
        std::lock_guard<std::mutex> lock(soft_usage_lock);
        if (soft_usage_text[0] && used < n) used += snprintf(buf + used, n - used, "\n  soft targets:%s", soft_usage_text);
    }
    if (total && used < n) {
        used += snprintf(buf + used, n - used, "\n  profile:");
        for (int z = 0; z < n64gfx::Z_COUNT && used < n; z++)
            if (counts[z] * 200 >= total) // >= 0.5%
                used += snprintf(buf + used, n - used, " %s %.1f%%", n64gfx::prof_zone_names[z], counts[z] * 100.0 / total);
    }
    // Racy reset from the stats thread; good enough for a 5 s profile.
}

namespace {
    // Alpha-tested fragment program for the cutout depth pass (PassState::alpha_test). vita2d's precompiled programs
    // cannot discard, so this one is compiled on the device with libshacccg (vitaShaRK), once, and cached as a GXP.
    // Without the compiler or a cached copy, cutouts keep their old behaviour (no depth writes).
    const char* const ALPHA_TEST_FS =
        "float4 main(float2 vTexcoord : TEXCOORD0, uniform sampler2D tex, uniform float4 uTintColor) : COLOR\n"
        "{\n"
        "    float4 c = tex2D(tex, vTexcoord) * uTintColor;\n"
        "    if (c.a < 0.5) discard;\n"
        "    return c;\n"
        "}\n";

    const SceGxmProgram* load_alpha_test_program() {
        uint32_t hash = 2166136261u;
        for (const char* c = ALPHA_TEST_FS; *c; c++) hash = (hash ^ (uint8_t)*c) * 16777619u;
        const char* dir = "ux0:data/GauntletLegends/shadercache";
        char path[128], flag[128];
        snprintf(path, sizeof(path), "%s/alphatest_f-%08x.gxp", dir, (unsigned)hash);
        snprintf(flag, sizeof(flag), "%s/compiling.flag", dir);
        if (FILE* f = fopen(path, "rb")) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            void* buf = n > 0 ? malloc((size_t)n) : nullptr;
            bool ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
            fclose(f);
            // The header holds the program size: a short (cut-off) file must not reach the shader patcher.
            if (ok && n >= 32 && sceGxmProgramCheck((const SceGxmProgram*)buf) >= 0 &&
                sceGxmProgramGetSize((const SceGxmProgram*)buf) == (unsigned)n) {
                vita_log("gxm: alpha-test shader loaded from %s\n", path);
                return (const SceGxmProgram*)buf;
            }
            free(buf);
            sceIoRemove(path);
            vita_log("gxm: alpha-test shader cache %s is invalid, recompiling\n", path);
        }
        SceIoStat st;
        // A compile that crashed inside libshacccg, or failed, leaves the flag behind: stay on the fallback from then
        // on instead of loading the compiler on every boot.
        if (sceIoGetstat(flag, &st) >= 0) {
            vita_log("gxm: alpha-test shader: an earlier compile failed or did not finish (delete %s to retry)\n", flag);
            return nullptr;
        }
        const char* module = nullptr;
        for (const char* m : { "ur0:data/libshacccg.suprx", "ur0:data/external/libshacccg.suprx" })
            if (!module && sceIoGetstat(m, &st) >= 0) module = m;
        if (!module || shark_init(module) < 0) {
            vita_log("gxm: alpha-test shader: no shader compiler (%s)\n", module ? "libshacccg failed to load" : "libshacccg.suprx not installed");
            return nullptr;
        }
        sceIoMkdir(dir, 0777);
        if (FILE* f = fopen(flag, "wb")) fclose(f);
        uint32_t size = (uint32_t)strlen(ALPHA_TEST_FS); // in: source length, out: program size
        SceGxmProgram* prog = shark_compile_shader_extended(ALPHA_TEST_FS, &size, SHARK_FRAGMENT_SHADER, SHARK_OPT_DEFAULT,
            SHARK_DISABLE, SHARK_DISABLE, SHARK_DISABLE);
        void* buf = nullptr;
        if (prog && size) {
            buf = malloc(size);
            if (buf) memcpy(buf, prog, size);
            shark_clear_output();
            shark_end();
        } else {
            vita_log("gxm: alpha-test shader failed to compile\n");
            if (const SceShaccCgCompileOutput* o = shark_get_internal_compile_output())
                for (int i = 0; i < o->diagnosticCount; i++)
                    if (o->diagnostics[i].message) vita_log("gxm:   %s\n", o->diagnostics[i].message);
            // The compiler's state is not trusted after a failure (HP2's first device build crashed cleaning up),
            // so it stays loaded, and the flag stays so later boots don't load it again.
            return nullptr;
        }
        if (!buf || sceGxmProgramCheck((const SceGxmProgram*)buf) < 0) { free(buf); return nullptr; }
        sceIoRemove(flag);
        if (FILE* f = fopen(path, "wb")) {
            bool written = fwrite(buf, 1, size, f) == size;
            if (fclose(f) != 0) written = false;
            if (!written) sceIoRemove(path);
        }
        vita_log("gxm: alpha-test shader compiled (%u bytes), cached as %s\n", (unsigned)size, path);
        return (const SceGxmProgram*)buf;
    }

    constexpr int SCALE = 2;
    constexpr int DISPLAY_W = 960, DISPLAY_H = 544;
    constexpr uint32_t VRING_SIZE = 8u << 20;
    constexpr uint32_t TEXPOOL_SIZE = 32u << 20;

    // First-fit allocator over one CDRAM block for the game's textures (vita2d gives every texture its own
    // 256 KB-aligned block, and Gauntlet keeps a few hundred small textures alive).
    class TexPool {
    public:
        bool init() {
            SceUID uid = sceKernelAllocMemBlock("n64gfx_tex", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, TEXPOOL_SIZE, nullptr);
            if (uid < 0) return false;
            sceKernelGetMemBlockBase(uid, (void**)&base);
            sceGxmMapMemory(base, TEXPOOL_SIZE, (SceGxmMemoryAttribFlags)(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE));
            free_list.push_back({ 0, TEXPOOL_SIZE });
            return true;
        }
        void* alloc(uint32_t size) {
            size = (size + 1023) & ~1023u;
            for (size_t i = 0; i < free_list.size(); i++) {
                if (free_list[i].size < size) continue;
                uint32_t off = free_list[i].off;
                free_list[i].off += size;
                free_list[i].size -= size;
                if (free_list[i].size == 0) free_list.erase(free_list.begin() + i);
                used += size;
                return base + off;
            }
            return nullptr;
        }
        void free(void* p, uint32_t size) {
            size = (size + 1023) & ~1023u;
            uint32_t off = (uint32_t)((uint8_t*)p - base);
            auto it = std::lower_bound(free_list.begin(), free_list.end(), off, [](const Range& r, uint32_t o) { return r.off < o; });
            it = free_list.insert(it, { off, size });
            size_t i = it - free_list.begin();
            if (i + 1 < free_list.size() && free_list[i].off + free_list[i].size == free_list[i + 1].off) {
                free_list[i].size += free_list[i + 1].size;
                free_list.erase(free_list.begin() + i + 1);
            }
            if (i > 0 && free_list[i - 1].off + free_list[i - 1].size == free_list[i].off) {
                free_list[i - 1].size += free_list[i].size;
                free_list.erase(free_list.begin() + i);
            }
            used -= size;
        }
        uint32_t used = 0;
    private:
        struct Range { uint32_t off, size; };
        uint8_t* base = nullptr;
        std::vector<Range> free_list;
    };

    struct GpuTex { SceGxmTexture gxm; void* mem; uint32_t bytes; };

    class GxmBackend final : public n64gfx::Backend {
    public:
        GxmBackend() {
            vita2d_init_advanced(1 << 20);
            vita2d_set_clear_color(0xFF000000);
            ctx = vita2d_get_context();
            SceGxmShaderPatcher* patcher = vita2d_get_shader_patcher();
            const SceGxmProgram* progs[5] = { &color_v_gxp_start, &color_f_gxp_start, &texture_v_gxp_start, &texture_f_gxp_start, &texture_tint_f_gxp_start };
            for (int i = 0; i < 5; i++) sceGxmShaderPatcherRegisterProgram(patcher, progs[i], &ids[i]);
            SceGxmVertexStream stream = { sizeof(n64gfx::Vertex), SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
            {
                const SceGxmProgramParameter* pos = sceGxmProgramFindParameterByName(&color_v_gxp_start, "aPosition");
                const SceGxmProgramParameter* col = sceGxmProgramFindParameterByName(&color_v_gxp_start, "aColor");
                SceGxmVertexAttribute a[2] = {
                    { 0, 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, (uint16_t)sceGxmProgramParameterGetResourceIndex(pos) },
                    { 0, 20, SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, (uint16_t)sceGxmProgramParameterGetResourceIndex(col) },
                };
                int err = sceGxmShaderPatcherCreateVertexProgram(patcher, ids[0], a, 2, &stream, 1, &vp_color);
                if (err < 0) vita_log("gxm: colour vertex program 0x%08X\n", err);
                wvp_color = sceGxmProgramFindParameterByName(&color_v_gxp_start, "wvp");
            }
            {
                const SceGxmProgramParameter* pos = sceGxmProgramFindParameterByName(&texture_v_gxp_start, "aPosition");
                const SceGxmProgramParameter* uv = sceGxmProgramFindParameterByName(&texture_v_gxp_start, "aTexcoord");
                SceGxmVertexAttribute a[2] = {
                    { 0, 0, SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, (uint16_t)sceGxmProgramParameterGetResourceIndex(pos) },
                    { 0, 12, SCE_GXM_ATTRIBUTE_FORMAT_F32, 2, (uint16_t)sceGxmProgramParameterGetResourceIndex(uv) },
                };
                int err = sceGxmShaderPatcherCreateVertexProgram(patcher, ids[2], a, 2, &stream, 1, &vp_tex);
                if (err < 0) vita_log("gxm: texture vertex program 0x%08X\n", err);
                wvp_tex = sceGxmProgramFindParameterByName(&texture_v_gxp_start, "wvp");
            }
            tint_param = sceGxmProgramFindParameterByName(&texture_tint_f_gxp_start, "uTintColor");
            if (const SceGxmProgram* at = load_alpha_test_program()) {
                at_tint = sceGxmProgramFindParameterByName(at, "uTintColor");
                at_ok = at_tint && sceGxmShaderPatcherRegisterProgram(patcher, at, &at_id) >= 0;
            }
            vita_log("gxm: cutout depth writes %s\n", at_ok ? "on" : "off (no alpha-test shader)");
            SceUID uid = sceKernelAllocMemBlock("n64gfx_vring", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, VRING_SIZE, nullptr);
            sceKernelGetMemBlockBase(uid, (void**)&vring);
            sceGxmMapMemory(vring, VRING_SIZE, SCE_GXM_MEMORY_ATTRIB_READ);
            indices = vita2d_get_linear_indices();
            if (!pool.init()) vita_log("gxm: texture pool allocation failed\n");
            vita_log("gxm: renderer ready\n");
        }

        void begin_target(uint32_t addr, int width, int height) override {
            end_scene();
            Target& t = targets[addr];
            int w = width * SCALE, h = height * SCALE;
            bool fresh = false;
            if (!t.tex || t.w != w || t.h != h) {
                if (t.tex) { vita2d_wait_rendering_done(); vita2d_free_texture(t.tex); }
                t.tex = vita2d_create_empty_texture_rendertarget(w, h, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
                t.w = w; t.h = h;
                t.n64_w = width; t.n64_h = height;
                if (!t.tex) { vita_log("gxm: render target %dx%d failed\n", w, h); return; }
                // Keep depth between scenes: a frame's draws can span several display lists.
                sceGxmDepthStencilSurfaceSetForceLoadMode(&t.tex->gxm_sfd, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
                sceGxmDepthStencilSurfaceSetForceStoreMode(&t.tex->gxm_sfd, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
                vita2d_texture_set_filters(t.tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
                vita_log("gxm: target %06X %dx%d\n", addr, w, h);
                fresh = true;
            }
            cur = &t;
            vita2d_start_drawing_advanced(t.tex, 0);
            in_scene = true;
            if (fresh) clear_target();
        }

        bool alpha_test() const override { return at_ok; }

        void draw(const n64gfx::PassState& s, const n64gfx::Vertex* v, int count) override {
            if (!in_scene || !cur || count <= 0) return;
            if (s.alpha_test && !at_ok) return;
            SceGxmFragmentProgram* fp = fragment_program(s);
            if (!fp) return;
            bool textured = s.prog != n64gfx::Prog::Color;
            sceGxmSetVertexProgram(ctx, textured ? vp_tex : vp_color);
            sceGxmSetFragmentProgram(ctx, fp);
            void* ub;
            sceGxmReserveVertexDefaultUniformBuffer(ctx, &ub);
            sceGxmSetUniformDataF(ub, textured ? wvp_tex : wvp_color, 0, 16, s.wvp);
            if (s.prog == n64gfx::Prog::TextureTint) {
                sceGxmReserveFragmentDefaultUniformBuffer(ctx, &ub);
                sceGxmSetUniformDataF(ub, s.alpha_test ? at_tint : tint_param, 0, 4, s.tint);
            }
            if (textured) {
                SceGxmTexture* base = texture(s.tex);
                if (!base) return;
                SceGxmTexture t = *base;
                SceGxmTextureFilter f = s.linear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
                sceGxmTextureSetMinFilter(&t, f);
                sceGxmTextureSetMagFilter(&t, f);
                static const SceGxmTextureAddrMode modes[3] = { SCE_GXM_TEXTURE_ADDR_REPEAT, SCE_GXM_TEXTURE_ADDR_MIRROR, SCE_GXM_TEXTURE_ADDR_CLAMP };
                sceGxmTextureSetUAddrMode(&t, modes[s.wrap_s]);
                sceGxmTextureSetVAddrMode(&t, modes[s.wrap_t]);
                sceGxmSetFragmentTexture(ctx, 0, &t);
            }
            sceGxmSetCullMode(ctx, SCE_GXM_CULL_NONE);
            SceGxmDepthFunc df = s.depth == n64gfx::D_ALWAYS ? SCE_GXM_DEPTH_FUNC_ALWAYS : s.depth == n64gfx::D_LESS ? SCE_GXM_DEPTH_FUNC_LESS : SCE_GXM_DEPTH_FUNC_LESS_EQUAL;
            sceGxmSetFrontDepthFunc(ctx, df);
            sceGxmSetFrontDepthWriteEnable(ctx, s.depth_write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
            sceGxmSetFrontDepthBias(ctx, s.decal ? -4 : 0, s.decal ? -4 : 0);
            sceGxmSetViewport(ctx, cur->w * 0.5f, cur->w * 0.5f, cur->h * 0.5f, -cur->h * 0.5f, 0.0f, 1.0f);
            int x0 = std::max(0, s.sx0 * SCALE), y0 = std::max(0, s.sy0 * SCALE);
            int x1 = std::min(cur->w, s.sx1 * SCALE), y1 = std::min(cur->h, s.sy1 * SCALE);
            if (x1 <= x0 || y1 <= y0) return;
            sceGxmSetRegionClip(ctx, SCE_GXM_REGION_CLIP_OUTSIDE, x0, y0, x1 - 1, y1 - 1);
            while (count > 0) {
                int n = std::min(count, 65535 / 3 * 3);
                uint32_t bytes = n * sizeof(n64gfx::Vertex);
                if (vring_used + bytes > VRING_SIZE) {
                    stall();
                    vring_used = 0;
                }
                void* dst = vring + vring_used;
                memcpy(dst, v, bytes);
                vring_used = (vring_used + bytes + 15) & ~15u;
                sceGxmSetVertexStream(ctx, 0, dst);
                sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, n);
                v += n;
                count -= n;
            }
        }

        // Debug (dump.go): the target's pixels (RGBA8, w x h at the 2x scale) after the GPU has finished.
        bool read_target(uint32_t addr, std::vector<uint32_t>& out, int& w, int& h) {
            auto it = targets.find(addr);
            if (it == targets.end() || !it->second.tex) return false;
            stall();
            w = it->second.w; h = it->second.h;
            const uint8_t* d = (const uint8_t*)vita2d_texture_get_datap(it->second.tex);
            unsigned stride = vita2d_texture_get_stride(it->second.tex);
            out.resize((size_t)w * h);
            for (int y = 0; y < h; y++) memcpy(out.data() + (size_t)y * w, d + (size_t)y * stride, w * 4);
            return true;
        }

        void forget_texture(uint32_t id) override {
            auto it = textures.find(id);
            if (it == textures.end()) return;
            dead.push_back(it->second);
            textures.erase(it);
        }

        void finish() override { end_scene(); }

        // Shows target `addr` (or, if 0, the CPU-drawn image in `cpu`) on the display.
        void present(uint32_t addr, const std::vector<uint32_t>* cpu, int cpu_w, int cpu_h) {
            end_scene();
            vita2d_texture* src = nullptr;
            float sw = 0, sh = 0;
            if (addr) {
                auto it = targets.find(addr);
                if (it != targets.end() && it->second.tex) { src = it->second.tex; sw = (float)it->second.w; sh = (float)it->second.h; }
            } else if (cpu && cpu_w > 0 && cpu_h > 0) {
                if (!cpu_tex || cpu_tex_w != cpu_w || cpu_tex_h != cpu_h) {
                    vita2d_wait_rendering_done();
                    if (cpu_tex) vita2d_free_texture(cpu_tex);
                    cpu_tex = vita2d_create_empty_texture_format(cpu_w, cpu_h, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
                    cpu_tex_w = cpu_w; cpu_tex_h = cpu_h;
                    vita2d_texture_set_filters(cpu_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
                } else {
                    vita2d_wait_rendering_done();
                }
                if (cpu_tex) {
                    uint8_t* d = (uint8_t*)vita2d_texture_get_datap(cpu_tex);
                    unsigned stride = vita2d_texture_get_stride(cpu_tex);
                    for (int y = 0; y < cpu_h; y++) memcpy(d + y * stride, cpu->data() + (size_t)y * cpu_w, cpu_w * 4);
                    src = cpu_tex; sw = (float)cpu_w; sh = (float)cpu_h;
                }
            }
            render_stage = 2;
            vita2d_start_drawing();
            vita2d_clear_screen();
            if (src) {
                // 4:3 picture, full height.
                float dw = DISPLAY_H * 4.0f / 3.0f;
                vita2d_draw_texture_scale(src, (DISPLAY_W - dw) * 0.5f, 0, dw / sw, DISPLAY_H / sh);
            }
            vita2d_end_drawing();
            render_stage = 3;
            vita2d_swap_buffers();
            render_stage = 4;
            // No wait for the GPU here: it keeps drawing this frame while the next display list is processed. The
            // vertex ring wraps with a full wait (stall), which is also when dropped textures are freed; GPU work
            // runs in submission order, and the display queue limits how far ahead the CPU gets.
            tex_pool_kb = pool.used >> 10;
            render_stage = 0;
        }

    private:
        struct Target { vita2d_texture* tex = nullptr; int w = 0, h = 0, n64_w = 0, n64_h = 0; };
        struct FpEntry { uint32_t key; SceGxmFragmentProgram* fp; };

        SceGxmContext* ctx = nullptr;
        SceGxmShaderPatcherId ids[5] = {};
        SceGxmVertexProgram* vp_color = nullptr;
        SceGxmVertexProgram* vp_tex = nullptr;
        const SceGxmProgramParameter* wvp_color = nullptr;
        const SceGxmProgramParameter* wvp_tex = nullptr;
        const SceGxmProgramParameter* tint_param = nullptr;
        SceGxmShaderPatcherId at_id = {};                   // alpha-test program (cutout depth pass)
        const SceGxmProgramParameter* at_tint = nullptr;
        bool at_ok = false;
        std::vector<FpEntry> fps;
        uint8_t* vring = nullptr;
        uint32_t vring_used = 0;
        const uint16_t* indices = nullptr;
        std::unordered_map<uint32_t, Target> targets;
        Target* cur = nullptr;
        bool in_scene = false;
        TexPool pool;
        std::unordered_map<uint32_t, GpuTex> textures;
        std::vector<GpuTex> dead;
        vita2d_texture* cpu_tex = nullptr;
        int cpu_tex_w = 0, cpu_tex_h = 0;

        // Waits for all GPU work. GXM can't finish inside a scene, so the open scene is ended and restarted
        // (colour and depth are kept: the depth surface has forced load/store).
        void stall() {
            bool reopen = in_scene;
            end_scene();
            vita2d_wait_rendering_done();
            vring_used = 0;
            for (const GpuTex& t : dead) pool.free(t.mem, t.bytes);
            dead.clear();
            if (reopen && cur && cur->tex) {
                vita2d_start_drawing_advanced(cur->tex, 0);
                in_scene = true;
            }
        }

        void end_scene() {
            if (!in_scene) return;
            vita2d_end_drawing();
            in_scene = false;
        }

        void clear_target() {
            n64gfx::PassState s;
            memset(s.wvp, 0, sizeof(s.wvp));
            s.wvp[0] = s.wvp[5] = s.wvp[10] = s.wvp[15] = 1;
            s.sx0 = s.sy0 = 0;
            s.sx1 = cur->n64_w; s.sy1 = cur->n64_h;
            s.depth = n64gfx::D_ALWAYS;
            s.depth_write = true;
            n64gfx::Vertex q[6] = {
                { -1, 1, 0.999f, 0, 0, 0, 0, 0, 255 }, { 1, 1, 0.999f, 0, 0, 0, 0, 0, 255 }, { 1, -1, 0.999f, 0, 0, 0, 0, 0, 255 },
                { -1, 1, 0.999f, 0, 0, 0, 0, 0, 255 }, { 1, -1, 0.999f, 0, 0, 0, 0, 0, 255 }, { -1, -1, 0.999f, 0, 0, 0, 0, 0, 255 },
            };
            draw(s, q, 6);
        }

        static SceGxmBlendFactor factor(n64gfx::Factor f) {
            switch (f) {
            case n64gfx::F_ZERO: return SCE_GXM_BLEND_FACTOR_ZERO;
            case n64gfx::F_ONE: return SCE_GXM_BLEND_FACTOR_ONE;
            case n64gfx::F_SRC_COLOR: return SCE_GXM_BLEND_FACTOR_SRC_COLOR;
            case n64gfx::F_INV_SRC_COLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case n64gfx::F_SRC_ALPHA: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
            case n64gfx::F_INV_SRC_ALPHA: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case n64gfx::F_DST_COLOR: return SCE_GXM_BLEND_FACTOR_DST_COLOR;
            case n64gfx::F_INV_DST_COLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case n64gfx::F_DST_ALPHA: return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
            case n64gfx::F_INV_DST_ALPHA: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            }
            return SCE_GXM_BLEND_FACTOR_ONE;
        }

        // Fragment program for (program, blend, colour mask), made on first use.
        SceGxmFragmentProgram* fragment_program(const n64gfx::PassState& s) {
            int p = (int)s.prog;
            uint32_t mask = ((s.mask & n64gfx::M_R) ? SCE_GXM_COLOR_MASK_R : 0) | ((s.mask & n64gfx::M_G) ? SCE_GXM_COLOR_MASK_G : 0) |
                ((s.mask & n64gfx::M_B) ? SCE_GXM_COLOR_MASK_B : 0) | ((s.mask & n64gfx::M_A) ? SCE_GXM_COLOR_MASK_A : 0);
            bool blend = s.blend || mask != SCE_GXM_COLOR_MASK_ALL;
            n64gfx::Factor src = s.blend ? s.src : n64gfx::F_ONE, dst = s.blend ? s.dst : n64gfx::F_ZERO;
            uint32_t key = p | (blend ? 1u << 4 : 0) | (s.alpha_test ? 1u << 5 : 0) | mask << 8 | (uint32_t)src << 16 | (uint32_t)dst << 24;
            for (const auto& e : fps) if (e.key == key) return e.fp;
            SceGxmBlendInfo bi{};
            bi.colorMask = (SceGxmColorMask)mask;
            bi.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
            bi.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
            bi.colorSrc = factor(src);
            bi.colorDst = factor(dst);
            bi.alphaSrc = factor(src);
            bi.alphaDst = factor(dst);
            static const int fid[3] = { 1, 3, 4 };
            const SceGxmProgram* vprog = p == 0 ? &color_v_gxp_start : &texture_v_gxp_start;
            SceGxmFragmentProgram* fp = nullptr;
            int err = sceGxmShaderPatcherCreateFragmentProgram(vita2d_get_shader_patcher(), s.alpha_test ? at_id : ids[fid[p]], SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                SCE_GXM_MULTISAMPLE_NONE, blend ? &bi : nullptr, vprog, &fp);
            if (err < 0) { vita_log("gxm: fragment program %08X failed 0x%08X\n", key, err); fp = nullptr; }
            fps.push_back({ key, fp });
            return fp;
        }

        SceGxmTexture* texture(const n64gfx::TexImage* img) {
            if (!img) return nullptr;
            auto it = textures.find(img->id);
            if (it == textures.end()) {
                // sceGxmTextureInitLinear implies a stride of ALIGN(width, 8) pixels.
                uint32_t stride = (img->w + 7) & ~7;
                GpuTex t{};
                t.bytes = stride * img->h * 4;
                t.mem = pool.alloc(t.bytes);
                if (!t.mem) {
                    // Pool full: wait for the GPU (which releases dropped textures) and try once more.
                    stall();
                    t.mem = pool.alloc(t.bytes);
                }
                if (!t.mem) {
                    static int warned = 0;
                    if (warned++ < 4) vita_log("gxm: texture pool full (%u KB used) for %dx%d\n", pool.used >> 10, img->w, img->h);
                    return nullptr;
                }
                for (int y = 0; y < img->h; y++) memcpy((uint8_t*)t.mem + y * stride * 4, img->px.data() + (size_t)y * img->w, img->w * 4);
                sceGxmTextureInitLinear(&t.gxm, t.mem, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, img->w, img->h, 0);
                it = textures.emplace(img->id, t).first;
            }
            return &it->second.gxm;
        }
    };

    class GxmContext final : public ultramodern::renderer::RendererContext {
    public:
        explicit GxmContext(uint8_t* rdram) : rdram(rdram), renderer(rdram, &backend, true) {
            setup_result = ultramodern::renderer::SetupResult::Success;
            renderer.debug_dir = "ux0:data/GauntletLegends/";
            SceUID prof = sceKernelCreateThread("n64gfx_prof", prof_thread, 0x10000100, 0x1000, 0, 0, nullptr);
            if (prof >= 0) sceKernelStartThread(prof, 0, nullptr);
        }
        bool valid() override { return true; }
        bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return true; }
        void enable_instant_present() override {}
        void send_dl(const OSTask* task) override {
            stat_dl_count++;
            render_stats = &renderer.stats;
            render_stage = 1;
            // The first few display lists go to log.txt (task fields and the first commands) for hardware debugging.
            static int logged = 0;
            if (logged < 4) {
                logged++;
                uint32_t dl = task->t.data_ptr & 0x7FFFFF;
                vita_log("dl %d: type %u flags %u ucode %08X data %08X size %u\n", logged, (unsigned)task->t.type,
                    (unsigned)task->t.flags, (unsigned)task->t.ucode, (unsigned)task->t.data_ptr, (unsigned)task->t.data_size);
                for (int i = 0; i < 16; i += 2)
                    vita_log("  %08X %08X\n", *(uint32_t*)(rdram + dl + i * 4), *(uint32_t*)(rdram + dl + i * 4 + 4));
            }
            // Debug: ux0:data/GauntletLegends/slowdl.txt (checked at start) delays each list by 20 ms, which gives
            // Vita3K the device's render time to test races between the game and the renderer.
            static const bool slow_dl = [] { FILE* f = fopen("ux0:data/GauntletLegends/slowdl.txt", "rb"); if (f) fclose(f); return f != nullptr; }();
            if (slow_dl) sceKernelDelayThread(20000);
            const uint32_t bad_before = renderer.stats.bad_cmds, draws_before = renderer.stats.draws;
            const uint32_t tris_before = renderer.stats.tris, gpu_before = renderer.stats.gpu_passes;
            SceUInt64 t0 = sceKernelGetProcessTimeWide();
            renderer.run(task->t.data_ptr & 0x3FFFFFF);
            SceUInt64 took = sceKernelGetProcessTimeWide() - t0;
            us_run += took;
            log_odd_list(task, renderer.stats.bad_cmds - bad_before, renderer.stats.draws - draws_before,
                renderer.stats.tris - tris_before, renderer.stats.gpu_passes - gpu_before, took);
            static int lists = 0;
            if (++lists == 150) {
                lists = 0;
                char text[160] = "";
                size_t k = 0;
                for (const auto& u : renderer.offscreen()->take_usage(3))
                    k += snprintf(text + k, sizeof(text) - k, " %06X %dx%d (%u draws, %u px)", u.addr, u.w, u.h, u.draws, u.pixels);
                std::lock_guard<std::mutex> lock(soft_usage_lock);
                memcpy(soft_usage_text, text, sizeof(text));
            }
            dl_serial++;
            render_stage = 0;
        }
        void update_screen() override {
            ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
            uint32_t origin = vi->VI_ORIGIN_REG;
            // Debug: ux0:data/GauntletLegends/dump.go writes the HUD buffer (RDRAM, RGBA16) to hud.raw.
            static int dump_check = 0;
            if (++dump_check % 30 == 0 && remove("ux0:data/GauntletLegends/dump.go") == 0) {
                if (FILE* g = fopen("ux0:data/GauntletLegends/n64trace.go", "wb")) fclose(g);
                if (FILE* f = fopen("ux0:data/GauntletLegends/hud.raw", "wb")) {
                    std::vector<uint32_t> hud;
                    renderer.decode_rdram(0x45F3E0, 416, 50, hud);
                    fwrite(hud.data(), 4, hud.size(), f);
                    fclose(f);
                }
                // Raw RGBA16 HUD words (coverage bit kept) and the displayed GPU target.
                if (FILE* f = fopen("ux0:data/GauntletLegends/hud16.raw", "wb")) {
                    for (int i = 0; i < 416 * 50; i++) {
                        uint32_t a = (0x45F3E0 + i * 2) & 0x7FFFFF;
                        uint16_t v = *(uint16_t*)(rdram + (a ^ 2));
                        fwrite(&v, 2, 1, f);
                    }
                    fclose(f);
                }
                std::vector<uint32_t> frame;
                int fw = 0, fh = 0;
                if (uint32_t t = renderer.gpu_target_for(origin); t && backend.read_target(t, frame, fw, fh))
                    if (FILE* f = fopen("ux0:data/GauntletLegends/frame.raw", "wb")) {
                        fwrite(&fw, 4, 1, f); fwrite(&fh, 4, 1, f);
                        fwrite(frame.data(), 4, frame.size(), f);
                        fclose(f);
                    }
            }
            // Nothing new since the last present (same buffer shown, no display list run): keep the picture on screen.
            // A present waits for the GPU and blocks the graphics thread, and the game often draws 20-30 frames a second.
            if (origin == shown_origin && dl_serial == shown_serial) { presents_same++; return; }
            shown_origin = origin;
            shown_serial = dl_serial;
            presents++;
            SceUInt64 t0 = sceKernelGetProcessTimeWide();
            n64gfx::prof_zone = n64gfx::Z_PRESENT;
            present_frame(vi, origin);
            n64gfx::prof_zone = n64gfx::Z_NONE;
            us_present += sceKernelGetProcessTimeWide() - t0;
        }

        // Black-flash hunting (log.txt, first 30 of each): a list with a bad command (where, the words read then and
        // the words there now: different means something rewrote the list while it was being drawn), and a list
        // with far fewer draws than usual.
        void log_odd_list(const OSTask* task, uint32_t bad, uint32_t draws, uint32_t tris, uint32_t gpu, SceUInt64 took) {
            static int bad_logged = 0, thin_logged = 0;
            static float avg_gpu = 0;
            const auto& st = renderer.stats;
            if (bad && bad_logged < 30) {
                bad_logged++;
                uint32_t a = st.bad_pc & 0x7FFFF8;
                uint32_t n0 = *(uint32_t*)(rdram + a), n1 = *(uint32_t*)(rdram + a + 4);
                vita_log("bad cmd: list %u root %06X pc %06X depth %u read %08X %08X now %08X %08X (%s), %u bad, %u draws, %.1f ms\n",
                    dl_serial, st.bad_root, a, st.bad_depth, st.bad_w0, st.bad_w1, n0, n1,
                    (n0 == st.bad_w0 && n1 == st.bad_w1) ? "same" : "CHANGED", bad, draws, took / 1000.0);
                for (int i = 0; i < 4; i++)
                    if (st.bad_branch[i][0] || st.bad_branch[i][1])
                        vita_log("    branch -%d: at %06X DE %08X -> %06X\n", i + 1, st.bad_branch[i][0], st.bad_branch[i][1], st.bad_branch[i][2]);
                for (int i = -4; i <= 3; i++) {
                    uint32_t p = (a + i * 8) & 0x7FFFF8;
                    vita_log("    %06X: %08X %08X\n", p, *(uint32_t*)(rdram + p), *(uint32_t*)(rdram + p + 4));
                }
            }
            // Only lists that draw into a displayed (GPU) target, compared by their GPU passes (fills and clears
            // included): HUD and other offscreen updates are small and drawn in software.
            if (gpu == 0) return;
            if (avg_gpu > 40 && gpu < avg_gpu * 0.15f && thin_logged < 30) {
                thin_logged++;
                vita_log("thin list: list %u data %08X gpu passes %u (usual %.0f) draws %u tris %u bad %u, %.1f ms, drew target %06X, shown target %06X\n", dl_serial,
                    (unsigned)task->t.data_ptr, gpu, avg_gpu, draws, tris, bad, took / 1000.0, st.gpu_target,
                    renderer.gpu_target_for(ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG));
                if (thin_logged <= 4) {
                    uint32_t dl = task->t.data_ptr & 0x7FFFF8;
                    for (int i = 0; i < 24; i += 2)
                        vita_log("    %08X %08X\n", *(uint32_t*)(rdram + dl + i * 4), *(uint32_t*)(rdram + dl + i * 4 + 4));
                }
            }
            avg_gpu += (gpu - avg_gpu) * 0.05f;
        }

        void present_frame(ultramodern::renderer::ViRegs* vi, uint32_t origin) {
            renderer.note_vi_origin(origin);
            uint32_t addr = renderer.gpu_target_for(origin);
            if (addr) {
                presents_gpu++;
                had_gpu_present = true;
                backend.present(addr, nullptr, 0, 0);
                return;
            }
            // Not a GPU target after GPU frames have been shown: log the first few (black-flash hunting).
            if (had_gpu_present && odd_presents_logged < 40) {
                odd_presents_logged++;
                vita_log("present: origin %08X not a GPU target (width %u, v_start %08X, y_scale %08X, status %08X)\n",
                    origin, (unsigned)vi->VI_WIDTH_REG, (unsigned)vi->VI_V_START_REG, (unsigned)vi->VI_Y_SCALE_REG,
                    (unsigned)vi->VI_STATUS_REG);
            }
            // CPU-drawn frame (or one drawn before the VI first showed it): show RDRAM.
            int w = (int)(vi->VI_WIDTH_REG & 0xFFF);
            uint32_t vs = vi->VI_V_START_REG;
            int h = (int)((((vs >> 16) & 0x3FF) < (vs & 0x3FF) ? ((vs & 0x3FF) - ((vs >> 16) & 0x3FF)) / 2 : 240) * (vi->VI_Y_SCALE_REG & 0xFFF) / 1024);
            if (w <= 0 || w > 640 || h <= 0 || h > 480 || origin == 0) { presents_blank++; backend.present(0, nullptr, 0, 0); return; }
            presents_rdram++;
            renderer.decode_rdram(origin, w, h, cpu_frame);
            backend.present(0, &cpu_frame, w, h);
        }
        void shutdown() override {}
        uint32_t get_display_framerate() const override { return 60; }
        float get_resolution_scale() const override { return (float)SCALE; }

    private:
        uint8_t* rdram;
        GxmBackend backend;
        n64gfx::Renderer renderer;
        std::vector<uint32_t> cpu_frame;
        uint32_t dl_serial = 0, shown_serial = ~0u, shown_origin = 0;
        bool had_gpu_present = false;
        int odd_presents_logged = 0;
    };
}

std::unique_ptr<ultramodern::renderer::RendererContext> gauntlet::vita::create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle, bool) {
    return std::make_unique<GxmContext>(rdram);
}
