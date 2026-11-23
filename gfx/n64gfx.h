// Small F3DEX2 / RDP renderer for Gauntlet Legends, built for GPUs that only offer a few fixed shader programs
// (the Vita without a runtime shader compiler): vertices are transformed, lit and run through the colour
// combiner on the CPU, and each N64 draw becomes one to four passes of three programs (vertex colour, texture,
// texture x tint) with ordinary blending. See n64gfx.cpp for the pass scheme.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace n64gfx {

// Shader input for every program: position (3 floats, transformed by PassState::wvp), uv, colour.
struct Vertex {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
};


// Sampling profiler support: the renderer keeps prof_zone set to the stage it is in, so another thread can sample
// it cheaply (Vita: perf.txt). Z_NONE = outside the renderer.
enum ProfZone : uint8_t { Z_NONE, Z_PARSE, Z_TRI, Z_VTX, Z_MTX, Z_FLUSH, Z_EMIT, Z_GPU, Z_SOFT, Z_TEX, Z_TMEM, Z_RECT,
    Z_TARGET, Z_PRESENT, Z_CFG, Z_COMB, Z_LIGHT, Z_COUNT };
extern volatile uint8_t prof_zone;
extern const char* const prof_zone_names[Z_COUNT];

enum class Prog : uint8_t { Color, Texture, TextureTint };
enum Factor : uint8_t { F_ZERO, F_ONE, F_SRC_COLOR, F_INV_SRC_COLOR, F_SRC_ALPHA, F_INV_SRC_ALPHA, F_DST_COLOR, F_INV_DST_COLOR, F_DST_ALPHA, F_INV_DST_ALPHA };
enum DepthFunc : uint8_t { D_ALWAYS, D_LESS, D_LEQUAL };
enum Wrap : uint8_t { W_REPEAT, W_MIRROR, W_CLAMP };
enum Mask : uint8_t { M_R = 1, M_G = 2, M_B = 4, M_A = 8, M_RGB = 7, M_ALL = 15 };

// A decoded texture (RGBA8, bytes R,G,B,A). Backends upload it on first use and keep it until forget_texture(id).
struct TexImage {
    uint32_t id;
    int w, h;
    std::vector<uint32_t> px;
    uint64_t key;
    uint32_t last_used;
};

struct PassState {
    Prog prog = Prog::Color;
    const TexImage* tex = nullptr;
    bool linear = false;
    Wrap wrap_s = W_CLAMP, wrap_t = W_CLAMP;
    float tint[4] = { 1, 1, 1, 1 };
    bool blend = false;
    Factor src = F_ONE, dst = F_ZERO; // used for both colour and alpha
    uint8_t mask = M_ALL;
    DepthFunc depth = D_ALWAYS;
    bool depth_write = false;
    bool decal = false;               // pull towards the viewer (depth bias)
    bool alpha_test = false;          // drop fragments whose texel alpha times tint alpha is below 0.5
    float wvp[16];                    // out[j] = sum_i (x, y, z, 1)[i] * wvp[i * 4 + j]
    int sx0, sy0, sx1, sy1;           // scissor in N64 pixels of the target, exclusive end
};

// Clip space after wvp: x and y in [-w, w] cover the target (y = +w is the top row), depth z/w in [0, 1] (clipped
// outside), compared (PassState::depth) against a buffer cleared to 1.
class Backend {
public:
    virtual ~Backend() = default;
    // Starts drawing into the target that stands for N64 colour image `addr` (width x height N64 pixels).
    virtual void begin_target(uint32_t addr, int width, int height) = 0;
    virtual void draw(const PassState& s, const Vertex* v, int count) = 0; // triangle list
    virtual void forget_texture(uint32_t id) = 0;
    virtual void finish() {}                                                // end of a display list
    virtual bool alpha_test() const { return true; }                        // PassState::alpha_test works
};

// Renders into N64 RGBA16 colour images in RDRAM (for buffers the game reads back, such as the HUD), or into
// RGBA8 memory images (PC comparison mode).
class SoftBackend final : public Backend {
public:
    SoftBackend(uint8_t* rdram, bool to_rdram) : rdram(rdram), to_rdram(to_rdram) {}
    void begin_target(uint32_t addr, int width, int height) override;
    void draw(const PassState& s, const Vertex* v, int count) override;
    void forget_texture(uint32_t) override {}
    // Memory mode: the image of a target (RGBA8), or nullptr.
    const std::vector<uint32_t>* image(uint32_t addr, int* w, int* h) const;
    bool has_target(uint32_t addr) const { return targets.count(addr) != 0; }
    // Profiling: per target, the draws and covered pixels since the last call (largest first, at most n).
    struct Usage { uint32_t addr; int w, h; uint32_t draws, pixels; };
    std::vector<Usage> take_usage(size_t n);

private:
    struct Target { int w = 0, h = 0; std::vector<uint32_t> px; };
    uint8_t* rdram;
    bool to_rdram;
    uint32_t cur_addr = 0;
    int cur_w = 0, cur_h = 0;
    Target* cur = nullptr;
    std::unordered_map<uint32_t, Target> targets;
    std::unordered_map<uint32_t, Usage> usage;
    Usage* cur_usage = nullptr;
    std::vector<float> depth;
    std::vector<uint8_t> scratch_alpha; // RGBA16 targets: cutout masks (same size as depth)
    int depth_w = 0, depth_h = 0;

    void raster(const PassState& s, const float (*cv)[10]);
    uint32_t read_px(int x, int y);
    void write_px(int x, int y, uint32_t c);
};

class Renderer {
public:
    // gpu draws the displayed framebuffers; colour images that never reach the screen are drawn in software,
    // into RDRAM (offscreen_to_rdram) or into memory images (when another renderer already writes RDRAM).
    Renderer(uint8_t* rdram, Backend* gpu, bool offscreen_to_rdram);
    ~Renderer();
    void run(uint32_t dl);
    // Called on every VI update with the VI origin; framebuffers the VI has shown are drawn on the GPU from then on.
    void note_vi_origin(uint32_t origin);
    // The colour image that holds `origin`, if the GPU has drawn it (else 0): then the caller presents that target.
    uint32_t gpu_target_for(uint32_t origin) const;
    // For CPU-drawn frames: RDRAM RGBA16 image at origin as RGBA8.
    void decode_rdram(uint32_t origin, int w, int h, std::vector<uint32_t>& out) const;
    SoftBackend* offscreen();
    std::string debug_dir;   // where n64trace.go / n64trace.txt live (debug draw trace)
    struct Stats { uint32_t tris, draws, passes, tex_decodes, textures, bad_cmds; uint32_t path_draws[8], path_passes[8], split_draws;
        // Last bad command: its address, words, the list run() started at and the call depth.
        uint32_t bad_pc, bad_w0, bad_w1, bad_root, bad_depth, bad_branch[4][3]; // + the last 4 G_DL (address, w1, target)
        uint32_t gpu_passes, gpu_target; // passes drawn into GPU (displayed) targets, the last one's target
        uint32_t list_waits, list_wait_timeouts; // branches to self waited out (the game finishing the list), gave up
    } stats{};

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace n64gfx
