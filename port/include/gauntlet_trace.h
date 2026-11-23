#ifndef __GAUNTLET_TRACE_H__
#define __GAUNTLET_TRACE_H__

// Function-entry tracing for the recompiled code (N64Recomp trace_mode). Build with GAUNTLET_TRACE
// to record the last entered functions per thread; the crash handler prints them.
#ifdef GAUNTLET_TRACE
#ifdef __cplusplus
extern "C" {
#endif
void gauntlet_trace_entry(const char* name);
#ifdef __cplusplus
}
#endif
#define TRACE_ENTRY() gauntlet_trace_entry(__func__);
#else
#define TRACE_ENTRY()
#endif
#define TRACE_RETURN()

#endif
