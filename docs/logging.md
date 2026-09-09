# Streaming diagnostics

Include `logger.h` in C++ modules and use `LOG_TRACE/DEBUG/INFO/WARN/ERROR`
with a module name and `event=... key=value` fields. Linux and C++11 are required.
Set `WEBSERVER_LOG_LEVEL=TRACE` before starting the process for per-packet details.
Supported levels: TRACE, DEBUG, INFO (default), WARN, ERROR, OFF.
The level is read once; restart to change it. Existing printf messages are not
controlled by this setting until migrated.

Each new log line contains UTC microsecond wall time, monotonic microseconds (`mono_us`), level, Linux thread ID and
module. Use `monotonic_us()` differences for `elapsed_us`; wall time can jump.
Preserve identifiers such as fd, packet pointer and capture sequence in events.
Pointers identify an object only during its lifetime; fd values may be reused.
Local send completion means acceptance by the kernel, not client playback.

Output goes to stderr, with bounded messages and an explicit truncation marker.
One fprintf emits each event; libc serializes stderr writes from these calls.
The logger preserves errno and does not evaluate disabled log arguments.
It is synchronous, not signal-handler safe, and TRACE can affect latency when
the output destination is slow. Redirect stderr to a file for diagnosis and use
INFO for normal operation. Rotation and asynchronous output are future work.

This first stage does not change capture ownership, packet formats or drop policy.

The implementation lives in src/logger.cpp. The server Makefiles already collect
src/*.cpp, so no source list change is needed. Standalone targets using the logger
must link logger.cpp as well; the existing media tools do not yet call it.
stream_log.h remains a compatibility include for existing server callers.

Plan progress: Commit 1 infrastructure is complete in code, pending Linux build
and runtime validation. Commit 2 has partial instrumentation; next complete the
accept/read/thread-pool/route path. Frame IDs, packet IDs and pipeline latency
tracing (Commits 3 and 4) have not been implemented. mono_us is a host-local clock
sample, not a frame capture timestamp or an end-to-end latency measurement.
