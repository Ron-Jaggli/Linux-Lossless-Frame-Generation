#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace lsfg {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

inline LogLevel g_log_level = LogLevel::Info;

inline void logv(LogLevel level, const char* tag, const char* fmt, va_list ap) {
    if (level < g_log_level)
        return;
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    static const char* names[] = {"DBG", "INF", "WRN", "ERR"};
    FILE* out = level >= LogLevel::Warn ? stderr : stdout;
    std::fprintf(out, "[%7ld.%03ld] %s %-8s ", ts.tv_sec, ts.tv_nsec / 1000000,
                 names[static_cast<int>(level)], tag);
    std::vfprintf(out, fmt, ap);
    std::fprintf(out, "\n");
    std::fflush(out);
}

#define LSFG_DEFINE_LOG(name, level)                                          \
    inline void name(const char* tag, const char* fmt, ...) {                 \
        va_list ap;                                                           \
        va_start(ap, fmt);                                                    \
        logv(level, tag, fmt, ap);                                            \
        va_end(ap);                                                           \
    }

LSFG_DEFINE_LOG(logDebug, LogLevel::Debug)
LSFG_DEFINE_LOG(logInfo, LogLevel::Info)
LSFG_DEFINE_LOG(logWarn, LogLevel::Warn)
LSFG_DEFINE_LOG(logError, LogLevel::Error)

#undef LSFG_DEFINE_LOG

inline double nowSeconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

} // namespace lsfg
