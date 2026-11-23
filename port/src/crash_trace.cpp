// Crash diagnostics: per-thread ring of recently entered recompiled functions + an exception handler
// that prints them along with the faulting address. GAUNTLET_DUMP=<seconds> also dumps every thread's
// ring after that many seconds (for finding hangs).
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <chrono>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include "gauntlet_trace.h"
#include <mutex>
#include <unordered_map>
#include <array>
#include <cmath>
#include <cstring>
#include "recomp.h"
extern uint8_t* gauntlet_rdram;

namespace {
    constexpr int ring_size = 32;
    constexpr int max_threads = 64;

    struct Ring {
        const char* names[ring_size];
        unsigned pos;
        DWORD thread_id;
        unsigned long long count;
    };
    Ring rings[max_threads];
    std::atomic<int> num_rings{ 0 };
    thread_local Ring* my_ring = nullptr;

    void print_ring(const Ring& r) {
        fprintf(stderr, "[trace] thread %lu (%llu calls), oldest first:\n", r.thread_id, r.count);
        for (int i = 0; i < ring_size; i++) {
            const char* n = r.names[(r.pos + i) % ring_size];
            if (n) fprintf(stderr, "  %s\n", n);
        }
    }

    // Walks the game's scheduler client list (OSSched at 0xE0127770, list head at +0x260).
    void dump_sched_clients() {
        if (!gauntlet_rdram) return;
        auto rd = [](uint32_t addr) { return *(uint32_t*)(gauntlet_rdram + (addr & 0x1FFFFFFF)); };
        auto str = [](uint32_t addr, char* out, int max) {
            int i = 0;
            for (; i < max - 1; i++) { char c = (char)gauntlet_rdram[((addr + i) ^ 3) & 0x1FFFFFFF]; if (!c) break; out[i] = c; }
            out[i] = 0;
        };
        char a[128], b[128];
        str(rd(0xE000BEBC), a, sizeof(a));
        str(0xE00AD630, b, sizeof(b));
        fprintf(stderr, "[fatal] '%s' '%s'\n", a, b);
        uint32_t node = rd(0xE0127770 + 0x260);
        for (int i = 0; i < 16 && node; i++) {
            fprintf(stderr, "[sched] client %08X: next=%08X mq=%08X\n", node, rd(node), rd(node + 4));
            node = rd(node);
        }
    }

    // GAUNTLET_WATCH=<vaddr>: report every write that changes that rdram word (page guard + single step).
    uint32_t watch_addr = 0;
    uint8_t* watch_page = nullptr;
    thread_local bool watch_stepping = false;
    thread_local uint32_t watch_old = 0;

    uint32_t* watch_ptr() { return (uint32_t*)(gauntlet_rdram + (watch_addr & 0x1FFFFFFF)); }

    void arm_watch() {
        DWORD old;
        VirtualProtect(watch_page, 4096, PAGE_READONLY, &old);
    }

    LONG WINAPI on_watch(EXCEPTION_POINTERS* info) {
        DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_ACCESS_VIOLATION && watch_page && info->ExceptionRecord->ExceptionInformation[0] == 1) {
            uint8_t* a = (uint8_t*)info->ExceptionRecord->ExceptionInformation[1];
            if (a >= watch_page && a < watch_page + 4096) {
                DWORD old;
                VirtualProtect(watch_page, 4096, PAGE_READWRITE, &old);
                watch_old = *watch_ptr();
                watch_stepping = true;
                if (a >= (uint8_t*)watch_ptr() && a < (uint8_t*)watch_ptr() + 64) {
                    HMODULE base = GetModuleHandleW(nullptr);
                    fprintf(stderr, "[guard] write to %p by thread %lu at exe+0x%llX\n", a, GetCurrentThreadId(),
                        (unsigned long long)((char*)info->ExceptionRecord->ExceptionAddress - (char*)base));
                }
                info->ContextRecord->EFlags |= 0x100; // trap after this instruction
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        if (code == EXCEPTION_SINGLE_STEP && watch_stepping) {
            watch_stepping = false;
            uint32_t now = *watch_ptr();
            if (now != watch_old) {
                fprintf(stderr, "[watch] %08X: %08X -> %08X\n", watch_addr, watch_old, now);
                if (my_ring) print_ring(*my_ring);
                fflush(stderr);
            }
            arm_watch();
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Suspend every other thread and print where it is executing (exe offsets, symbolize with llvm-symbolizer).
    void snapshot_threads() {
        DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
        HMODULE base = GetModuleHandleW(nullptr);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{ sizeof(te) };
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!h) continue;
            SuspendThread(h);
            CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &c)) {
                HMODULE mod = nullptr; char name[MAX_PATH] = "?";
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)c.Rip, &mod);
                if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
                const char* shortname = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
                if (_stricmp(shortname, "ntdll.dll") != 0 && _stricmp(shortname, "win32u.dll") != 0)
                    fprintf(stderr, "   native %lu rip %s+0x%llX\n", te.th32ThreadID, shortname, (unsigned long long)(c.Rip - (DWORD64)mod));
            }
            ResumeThread(h);
            CloseHandle(h);
        }
        CloseHandle(snap);
    }

    LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
        DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
            code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        HMODULE base = GetModuleHandleW(nullptr);
        void* pc = info->ExceptionRecord->ExceptionAddress;
        fprintf(stderr, "\n[crash] exception %08lX at %p (exe+0x%llX)", code, pc, (unsigned long long)((char*)pc - (char*)base));
        if (code == EXCEPTION_ACCESS_VIOLATION) {
            fprintf(stderr, " %s address %p", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (void*)info->ExceptionRecord->ExceptionInformation[1]);
        }
        fprintf(stderr, "\n");
        if (my_ring) print_ring(*my_ring);
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    struct Installer {
        Installer() {
            AddVectoredExceptionHandler(1, on_exception);
            if (const char* g = std::getenv("GAUNTLET_GUARD")) {
                AddVectoredExceptionHandler(1, on_watch);
                watch_addr = (uint32_t)strtoul(g, nullptr, 16);
                std::thread([] {
                    while (!gauntlet_rdram) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    watch_page = (uint8_t*)((uintptr_t)watch_ptr() & ~(uintptr_t)4095);
                    arm_watch();
                }).detach();
            }
            else if (const char* w = std::getenv("GAUNTLET_WATCH")) {
                // Polling watch: on change, print every thread's most recent calls (the writer is among them).
                watch_addr = (uint32_t)strtoul(w, nullptr, 16);
                std::thread([] {
                    while (!gauntlet_rdram) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    volatile uint32_t* p = watch_ptr();
                    uint32_t last = *p;
                    for (int reports = 0; reports < 12;) {
                        uint32_t now = *p;
                        if (now != last) {
                            snapshot_threads();
                            fprintf(stderr, "[watch] %08X: %08X -> %08X\n", watch_addr, last, now);
                            for (int i = 0; i < num_rings.load(); i++) {
                                const Ring& r = rings[i];
                                fprintf(stderr, "   thread %lu:", r.thread_id);
                                for (int k = 16; k >= 1; k--) {
                                    const char* n = r.names[(r.pos - k) % ring_size];
                                    fprintf(stderr, " %s |", n ? n : "-");
                                }
                                fprintf(stderr, "\n");
                            }
                            fflush(stderr);
                            last = now; reports++;
                        }
                    }
                }).detach();
            }
            if (const char* s = std::getenv("GAUNTLET_DUMP")) {
                int secs = std::atoi(s);
                std::thread([secs] {
                    for (int round = 0; round < 2; round++) {
                        std::this_thread::sleep_for(std::chrono::seconds(secs));
                        fprintf(stderr, "[trace] ---- dump %d ----\n", round);
                        for (int i = 0; i < num_rings.load(); i++) print_ring(rings[i]);
                        dump_sched_clients();
                        fflush(stderr);
                    }
                }).detach();
            }
        }
    } installer;
}

extern "C" void gauntlet_trace_entry(const char* name) {
    if (!my_ring) {
        int i = num_rings.fetch_add(1);
        if (i >= max_threads) { num_rings.store(max_threads); return; }
        my_ring = &rings[i];
        my_ring->thread_id = GetCurrentThreadId();
    }
    my_ring->names[my_ring->pos++ % ring_size] = name;
    my_ring->count++;
    // GAUNTLET_COUNT=<from>,<to> (seconds): count calls per function in that window and print them afterwards.
    static const char* count_env = getenv("GAUNTLET_COUNT");
    if (count_env != nullptr) {
        static std::mutex count_mutex;
        static std::unordered_map<const char*, uint32_t> counts;
        static const auto start = std::chrono::steady_clock::now();
        static double from = atof(count_env), to = atof(strchr(count_env, ',') ? strchr(count_env, ',') + 1 : count_env);
        static bool printed = false;
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::lock_guard<std::mutex> lock(count_mutex);
        if (t >= from && t < to) counts[name]++;
        else if (t >= to && !printed) {
            printed = true;
            for (auto& [n, c] : counts) fprintf(stderr, "[count] %8u %s\n", c, n);
            fflush(stderr);
        }
    }
}

// Records "<what> a b" in the ring (used for message queue calls from the runtime).
extern "C" void gauntlet_trace_note(const char* what, uint32_t a, uint32_t b) {
    thread_local char notes[ring_size][48];
    thread_local unsigned note_pos = 0;
    char* buf = notes[note_pos++ % ring_size];
    snprintf(buf, 48, "%s %08X %X", what, a, b);
    gauntlet_trace_entry(buf);
}

// Debug: GAUNTLET_MTXLOG records (without locks or output) every call of the game's 4x3 float matrix converters
// (func_E002BFE8 dst=a1, func_E002C0F8 dst=a2) that targets GAUNTLET_MTXLOG's address; printed by the DL dumper.
struct MtxRecord { uint32_t serial; uint32_t func_dst_reg; uint32_t src; float m[12]; };
static MtxRecord mtx_records[16];
static volatile uint32_t mtx_record_pos = 0;
static uint32_t mtx_converter_calls = 0;

extern "C" void gauntlet_debug_mtxf2l(uint8_t* rdram, recomp_context* ctx) {}

extern "C" void gauntlet_debug_mtx43(uint8_t* rdram, recomp_context* ctx) {
    static const char* env = getenv("GAUNTLET_MTXLOG");
    static const uint32_t target = env ? (uint32_t)strtoul(env, nullptr, 16) & 0xFFFFFF : 0;
    if (env == nullptr) return;
    mtx_converter_calls++;
    uint32_t a1 = (uint32_t)ctx->r5, a2 = (uint32_t)ctx->r6;
    uint32_t reg = 0;
    if ((a1 & 0xFFFFFF) == target && (a1 >> 24) >= 0x80) reg = 5;
    else if ((a2 & 0xFFFFFF) == target && (a2 >> 24) >= 0x80) reg = 6;
    if (reg == 0) return;
    MtxRecord& r = mtx_records[mtx_record_pos % 16];
    r.serial = mtx_converter_calls;
    r.func_dst_reg = reg;
    r.src = (uint32_t)ctx->r4;
    for (int i = 0; i < 12; i++) { int32_t w = MEM_W(i * 4, ctx->r4); memcpy(&r.m[i], &w, 4); }
    mtx_record_pos = mtx_record_pos + 1;
}

extern "C" void gauntlet_print_mtx_records(FILE* f) {
    fprintf(f, "converter calls %u, records %u\n", mtx_converter_calls, mtx_record_pos);
    uint32_t end = mtx_record_pos, start = end > 16 ? end - 16 : 0;
    for (uint32_t k = start; k < end; k++) {
        const MtxRecord& r = mtx_records[k % 16];
        fprintf(f, "  #%u src %08X dst in a%u:", r.serial, r.src, r.func_dst_reg);
        for (int i = 0; i < 12; i++) fprintf(f, "%s%.4f", (i % 3) ? " " : " | ", r.m[i]);
        fprintf(f, "\n");
    }
}

// Debug: GAUNTLET_HWWATCH=<vaddr> sets a hardware write breakpoint (DR0) on that rdram word in every thread
// and logs the host instruction after each write (symbolize with llvm-symbolizer).
namespace {
    LONG WINAPI on_hwwatch(EXCEPTION_POINTERS* info) {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || (info->ContextRecord->Dr6 & 1) == 0) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        static std::atomic<int> logs{0};
        info->ContextRecord->Dr6 = 0;
        uint32_t now = *(uint32_t*)info->ContextRecord->Dr0;
        float nowf; memcpy(&nowf, &now, 4);
        static const bool big_only = getenv("GAUNTLET_HWWATCH_BIG") != nullptr;
        if (big_only && !(fabsf(nowf) > 1.5f)) return EXCEPTION_CONTINUE_EXECUTION;
        if (logs.fetch_add(1) < 60) {
            HMODULE base = GetModuleHandleW(nullptr);
            fprintf(stderr, "[hw] write %08X by thread %lu at exe+0x%llX\n", now, GetCurrentThreadId(),
                (unsigned long long)((char*)info->ExceptionRecord->ExceptionAddress - (char*)base));
            fflush(stderr);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    struct HwWatchInstaller {
        HwWatchInstaller() {
            const char* w = std::getenv("GAUNTLET_HWWATCH");
            if (w == nullptr) return;
            uint32_t vaddr = (uint32_t)strtoul(w, nullptr, 16);
            AddVectoredExceptionHandler(1, on_hwwatch);
            std::thread([vaddr] {
                while (!gauntlet_rdram) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                uintptr_t addr = (uintptr_t)(gauntlet_rdram + (vaddr & 0x1FFFFFFF));
                DWORD self = GetCurrentThreadId();
                for (;;) {
                    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                    THREADENTRY32 te{ sizeof(te) };
                    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
                        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                        if (!t) continue;
                        SuspendThread(t);
                        CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                        if (GetThreadContext(t, &c) && c.Dr0 != addr) {
                            c.Dr0 = addr;
                            c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18); // L0, write, 4 bytes
                            SetThreadContext(t, &c);
                        }
                        ResumeThread(t);
                        CloseHandle(t);
                    }
                    CloseHandle(snap);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            }).detach();
        }
    } hw_watch_installer;
}

// Debug: inputs of the 3x3 multiply func_E0013FC8(A=a0, B=a1, C=a2) when C is GAUNTLET_MULLOG's address.
struct MulRecord { uint32_t a, b; float A[9], B[9]; };
static MulRecord mul_records[8];
static volatile uint32_t mul_record_pos = 0;
extern "C" void gauntlet_debug_mul33(uint8_t* rdram, recomp_context* ctx) {
    static const char* env = getenv("GAUNTLET_MULLOG");
    static const uint32_t target = env ? (uint32_t)strtoul(env, nullptr, 16) & 0xFFFFFF : 0;
    if (env == nullptr || ((uint32_t)ctx->r6 & 0xFFFFFF) != target) return;
    MulRecord& r = mul_records[mul_record_pos % 8];
    r.a = (uint32_t)ctx->r4; r.b = (uint32_t)ctx->r5;
    for (int i = 0; i < 9; i++) {
        int32_t wa = MEM_W(i * 4, ctx->r4), wb = MEM_W(i * 4, ctx->r5);
        memcpy(&r.A[i], &wa, 4); memcpy(&r.B[i], &wb, 4);
    }
    mul_record_pos = mul_record_pos + 1;
}
extern "C" void gauntlet_print_mul_records(FILE* f) {
    uint32_t end = mul_record_pos, start = end > 8 ? end - 8 : 0;
    for (uint32_t k = start; k < end; k++) {
        const MulRecord& r = mul_records[k % 8];
        fprintf(f, "  mul A %08X:", r.a);
        for (int i = 0; i < 9; i++) fprintf(f, "%s%.4f", (i % 3) ? " " : " | ", r.A[i]);
        fprintf(f, "\n        B %08X:", r.b);
        for (int i = 0; i < 9; i++) fprintf(f, "%s%.4f", (i % 3) ? " " : " | ", r.B[i]);
        fprintf(f, "\n");
    }
}
