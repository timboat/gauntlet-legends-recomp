#ifndef __VITA_LOG_H__
#define __VITA_LOG_H__

#include <atomic>
#include <cstdint>

// Appends a line to ux0:data/GauntletLegends/log.txt.
void vita_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

extern std::atomic<uint32_t> stat_dl_count;
extern std::atomic<uint32_t> stat_vi_count;

#endif
