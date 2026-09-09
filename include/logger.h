#ifndef WEBSERVER_CAMERA_LOGGER_H
#define WEBSERVER_CAMERA_LOGGER_H

#include <cerrno>

namespace stream_log {
enum Level { TRACE, DEBUG, INFO, WARN, ERROR, OFF };

// Configuration is initialized once, using C++11 thread-safe static initialization.
Level configured_level();
unsigned long long monotonic_us();

// Linux/GCC-compatible printf checking; implementation preserves errno.
// One bounded fprintf call serializes each event through the stderr FILE lock.
void write(Level level, const char* module, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
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
