// Keyboard + SDL game controller input mapped to the N64 controller.
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstring>
#include <vector>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "SDL.h"
#include "gauntlet_game.h"

namespace {
    enum : uint16_t {
        N64_A = 0x8000, N64_B = 0x4000, N64_Z = 0x2000, N64_START = 0x1000,
        N64_DU = 0x0800, N64_DD = 0x0400, N64_DL = 0x0200, N64_DR = 0x0100,
        N64_L = 0x0020, N64_R = 0x0010,
        N64_CU = 0x0008, N64_CD = 0x0004, N64_CL = 0x0002, N64_CR = 0x0001,
    };

    std::mutex controllers_mutex;
    std::vector<SDL_GameController*> controllers;

    float axis(SDL_GameController* c, SDL_GameControllerAxis a) {
        float v = SDL_GameControllerGetAxis(c, a) / 32767.0f;
        if (std::fabs(v) < 0.15f) return 0.0f;
        return std::fmax(-1.0f, std::fmin(1.0f, v));
    }
}

// Test aid: GAUNTLET_AUTOINPUT=<file> with lines "<seconds> <hex buttons> [<ms held, default 150>]"
// presses those N64 buttons on controller 1 at that time after start.
namespace {
    struct AutoPress { double t; uint16_t buttons; double held; float x, y; };
    std::vector<AutoPress> auto_presses;
    std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();

    void load_autoinput() {
        const char* path = std::getenv("GAUNTLET_AUTOINPUT");
        if (!path) return;
        FILE* f = std::fopen(path, "r");
        if (!f) return;
        double t, held; unsigned b; float x, y; char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            held = 150; x = 0; y = 0;
            if (std::sscanf(line, "%lf %x %lf %f %f", &t, &b, &held, &x, &y) >= 2) auto_presses.push_back({ t, (uint16_t)b, held / 1000.0, x, y });
        }
        std::fclose(f);
        fprintf(stderr, "[input] %zu scripted presses\n", auto_presses.size());
    }

    // Test aid: GAUNTLET_LIVEINPUT=<file> is polled while the game runs; each line appended to it,
    // "<hex buttons> [<ms held>] [<x> <y>]", is pressed on controller 1 right away.
    long live_offset = 0;
    double live_next_poll = 0;
    void poll_liveinput(double now) {
        static const char* path = std::getenv("GAUNTLET_LIVEINPUT");
        if (!path || now < live_next_poll) return;
        live_next_poll = now + 0.1;
        FILE* f = std::fopen(path, "r");
        if (!f) return;
        std::fseek(f, live_offset, SEEK_SET);
        char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            if (!std::strchr(line, '\n')) break;
            live_offset = std::ftell(f);
            unsigned b; double held = 150; float x = 0, y = 0;
            if (std::sscanf(line, "%x %lf %f %f", &b, &held, &x, &y) >= 1) {
                auto_presses.push_back({ now + 0.05, (uint16_t)b, held / 1000.0, x, y });
                fprintf(stderr, "[input] live %04X %.0f ms\n", b, held);
            }
        }
        std::fclose(f);
    }

    float auto_x = 0, auto_y = 0;
    std::mutex auto_mutex;
    uint16_t auto_buttons() {
        std::lock_guard lock{ auto_mutex };
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        poll_liveinput(now);
        uint16_t b = 0;
        auto_x = auto_y = 0;
        for (const auto& p : auto_presses) {
            if (now >= p.t && now < p.t + p.held) { b |= p.buttons; if (p.x || p.y) { auto_x = p.x; auto_y = p.y; } }
        }
        return b;
    }
}

void gauntlet::input::init() {
    load_autoinput();
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            if (SDL_GameController* c = SDL_GameControllerOpen(i)) controllers.push_back(c);
        }
    }
}

void gauntlet::input::handle_event(const void* ev) {
    const SDL_Event* e = static_cast<const SDL_Event*>(ev);
    std::lock_guard lock{ controllers_mutex };
    if (e->type == SDL_CONTROLLERDEVICEADDED) {
        // SDL also sends this at start-up for pads init() already opened; opening again returns the same
        // handle, which would put one pad in two player slots.
        if (SDL_GameController* c = SDL_GameControllerOpen(e->cdevice.which)) {
            if (std::find(controllers.begin(), controllers.end(), c) == controllers.end()) controllers.push_back(c);
            else SDL_GameControllerClose(c);
        }
    }
    else if (e->type == SDL_CONTROLLERDEVICEREMOVED) {
        for (auto it = controllers.begin(); it != controllers.end(); ++it) {
            if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(*it)) == e->cdevice.which) {
                SDL_GameControllerClose(*it);
                controllers.erase(it);
                break;
            }
        }
    }
}

void gauntlet::input::poll() {}

// Player 1 gets the keyboard plus the first pad; players 2-4 get the next pads (Gauntlet is 4-player).
bool gauntlet::input::get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    uint16_t b = 0;
    float sx = 0.0f, sy = 0.0f;

    if (controller_num == 0) {
        uint16_t ab = auto_buttons();
        static uint16_t last_ab = 0;
        if (ab != last_ab) { fprintf(stderr, "[input] scripted buttons %04X\n", ab); last_ab = ab; }
        b |= ab;
        sx += auto_x; sy += auto_y;
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        if (k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_J]) b |= N64_A;          // attack
        if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_K]) b |= N64_B;         // magic / turbo
        if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_L]) b |= N64_Z;
        if (k[SDL_SCANCODE_RETURN]) b |= N64_START;
        if (k[SDL_SCANCODE_Q]) b |= N64_L;
        if (k[SDL_SCANCODE_E]) b |= N64_R;
        if (k[SDL_SCANCODE_I]) b |= N64_CU;
        if (k[SDL_SCANCODE_COMMA]) b |= N64_CD;
        if (k[SDL_SCANCODE_U]) b |= N64_CL;
        if (k[SDL_SCANCODE_O]) b |= N64_CR;
        if (k[SDL_SCANCODE_UP]) b |= N64_DU;
        if (k[SDL_SCANCODE_DOWN]) b |= N64_DD;
        if (k[SDL_SCANCODE_LEFT]) b |= N64_DL;
        if (k[SDL_SCANCODE_RIGHT]) b |= N64_DR;
        if (k[SDL_SCANCODE_A]) sx -= 1.0f;
        if (k[SDL_SCANCODE_D]) sx += 1.0f;
        if (k[SDL_SCANCODE_W]) sy += 1.0f;
        if (k[SDL_SCANCODE_S]) sy -= 1.0f;
    }

    {
        std::lock_guard lock{ controllers_mutex };
        if ((size_t)controller_num < controllers.size()) {
            SDL_GameController* c = controllers[controller_num];
            auto btn = [&](SDL_GameControllerButton sb, uint16_t nb) { if (SDL_GameControllerGetButton(c, sb)) b |= nb; };
            btn(SDL_CONTROLLER_BUTTON_A, N64_A);
            btn(SDL_CONTROLLER_BUTTON_X, N64_B);
            btn(SDL_CONTROLLER_BUTTON_B, N64_CD);
            btn(SDL_CONTROLLER_BUTTON_Y, N64_CU);
            btn(SDL_CONTROLLER_BUTTON_START, N64_START);
            btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, N64_L);
            btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, N64_R);
            btn(SDL_CONTROLLER_BUTTON_DPAD_UP, N64_DU);
            btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN, N64_DD);
            btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT, N64_DL);
            btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, N64_DR);
            if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) b |= N64_Z;
            if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) b |= N64_R;
            float rx = axis(c, SDL_CONTROLLER_AXIS_RIGHTX), ry = axis(c, SDL_CONTROLLER_AXIS_RIGHTY);
            if (rx < -0.5f) b |= N64_CL;
            if (rx > 0.5f) b |= N64_CR;
            if (ry < -0.5f) b |= N64_CU;
            if (ry > 0.5f) b |= N64_CD;
            float lx = axis(c, SDL_CONTROLLER_AXIS_LEFTX), ly = -axis(c, SDL_CONTROLLER_AXIS_LEFTY);
            if (lx != 0.0f || ly != 0.0f) { sx = lx; sy = ly; }
        }
        else if (controller_num != 0) {
            return false;
        }
    }

    *buttons = b;
    *x = sx;
    *y = sy;
    return true;
}

void gauntlet::input::set_rumble(int controller_num, bool rumble) {
    std::lock_guard lock{ controllers_mutex };
    if ((size_t)controller_num < controllers.size()) {
        SDL_GameControllerRumble(controllers[controller_num], rumble ? 0xFFFF : 0, rumble ? 0xFFFF : 0, rumble ? 1000 : 0);
    }
}
