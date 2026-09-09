#include "logger.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/syscall.h>
#include <unistd.h>

namespace stream_log {
Level configured_level() {
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

unsigned long long monotonic_us() {
    const int saved_errno = errno;
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    errno = saved_errno;
    return static_cast<unsigned long long>(now.tv_sec) * 1000000ULL + now.tv_nsec / 1000;
}

void write(Level level, const char* module, const char* format, ...) {
    const int saved_errno = errno;
    if (level < TRACE || level >= OFF || level < configured_level()) {
        errno = saved_errno;
        return;
    }
    const unsigned long long mono_us = monotonic_us();
    timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    tm utc = {};
    gmtime_r(&now.tv_sec, &utc);
    char timestamp[32] = {};
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &utc);
    char message[2048] = {};
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
                 "%s.%06ldZ mono_us=%llu level=%s tid=%ld module=%s %s%s\n",
                 timestamp, now.tv_nsec / 1000, mono_us, names[level],
                 static_cast<long>(syscall(SYS_gettid)), module,
                 length < 0 ? "event=format_error" : message,
                 length >= static_cast<int>(sizeof(message)) ? " truncated=true" : "");
    errno = saved_errno;
}
}
