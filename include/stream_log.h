#ifndef STREAM_LOG_H
#define STREAM_LOG_H

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/syscall.h>
#include <unistd.h>

// Header-only so standalone media tools can use the same logger.
namespace stream_log {
enum Level { TRACE, DEBUG, INFO, WARN, ERROR, OFF };

inline Level configured_level() {
    static const Level level = []() {
        const char* value = std::getenv("WEBSERVER_LOG_LEVEL");
        const char* names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "OFF"};
        for (int i = TRACE; value && i <= OFF; ++i) {
            if (std::strcmp(value, names[i]) == 0) return static_cast<Level>(i);
        }
        return INFO;
    }();
    return level;
}

inline unsigned long long monotonic_us() {
    const int saved_errno = errno;
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    errno = saved_errno;
    return static_cast<unsigned long long>(now.tv_sec) * 1000000ULL + now.tv_nsec / 1000;
}

inline void write(Level level, const char* module, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

inline void write(Level level, const char* module, const char* format, ...) {
    const int saved_errno = errno;
    timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    tm utc = {};
    gmtime_r(&now.tv_sec, &utc);
    char timestamp[32] = {};
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &utc);
    char message[2048];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    // Keep one event on one line, even when adapting legacy messages.
    for (char* p = message; length >= 0 && *p; ++p) {
        if (*p == '\n' || *p == '\r') *p = ' ';
    }
    const char* names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "OFF"};
    std::fprintf(stderr,
                 "%s.%06ldZ level=%s tid=%ld module=%s %s%s\n",
                 timestamp, now.tv_nsec / 1000, names[level],
                 static_cast<long>(syscall(SYS_gettid)), module,
                 length < 0 ? "event=format_error" : message,
                 length >= static_cast<int>(sizeof(message)) ? " truncated=true" : "");
    errno = saved_errno;
}
}

// Disabled log arguments are not evaluated; preserve errno even on first use.
#define STREAM_LOG(level, module, ...) do { \
    const int stream_log_saved_errno = errno; \
    const bool stream_log_enabled = (level) >= stream_log::configured_level(); \
    errno = stream_log_saved_errno; \
    if (stream_log_enabled) stream_log::write(level, module, __VA_ARGS__); \
    errno = stream_log_saved_errno; \
} while (0)
#define LOG_TRACE(module, ...) STREAM_LOG(stream_log::TRACE, module, __VA_ARGS__)
#define LOG_DEBUG(module, ...) STREAM_LOG(stream_log::DEBUG, module, __VA_ARGS__)
#define LOG_INFO(module, ...) STREAM_LOG(stream_log::INFO, module, __VA_ARGS__)
#define LOG_WARN(module, ...) STREAM_LOG(stream_log::WARN, module, __VA_ARGS__)
#define LOG_ERROR(module, ...) STREAM_LOG(stream_log::ERROR, module, __VA_ARGS__)

#endif
