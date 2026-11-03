#include "cv_frame_bus.h"
#include "pipeline_metrics.h"
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <atomic>
#include <mutex>
static std::atomic<unsigned> cv_mode{0};
bool cv_set_mode(unsigned mode) {
    if(mode>3) return false;
    static std::mutex control;
    std::lock_guard<std::mutex> guard(control);
    FILE* f=fopen("/tmp/webserver_cv.control.next","w");
    if(!f) return false;
    bool ok=fprintf(f,"%u\n",mode)>0;
    if(fclose(f)) ok=false;
    if(ok) ok=rename("/tmp/webserver_cv.control.next","/tmp/webserver_cv.control")==0;
    if(ok) cv_mode.store(mode);
    return ok;
}

void cv_submit_nv12(const unsigned char* data, unsigned w, unsigned h, uint64_t stamp) {
    static const bool enabled = getenv("CV_ENABLE") && std::string(getenv("CV_ENABLE")) == "1";
    if (!enabled || !cv_mode.load() || !data || !w || !h || (w & 1) || (h & 1)) return;
    const size_t bytes = size_t(w) * h * 3 / 2;
    if (bytes > CV_MAX_BYTES) return;
    static int fd = -1;
    static uint64_t next = 0, sequence = 0;
    const uint64_t now = perf_now_us();
    if (now < next) return;
    next = now + 40000; // Up to 25 fps publish; slower NPU consumes latest frame.
    if (fd < 0) fd = open("/tmp/webserver_cv.frame", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) { perf_count("cv_publish_error"); return; }
    if (flock(fd, LOCK_EX | LOCK_NB)) { perf_count("cv_publish_skip"); return; }
    CvFrameHeader header = {0, w, h, static_cast<uint32_t>(bytes), ++sequence, stamp};
    bool ok = pwrite(fd, &header, sizeof(header), 0) == ssize_t(sizeof(header));
    if (ok) ok = pwrite(fd, data, bytes, sizeof(header)) == ssize_t(bytes);
    if (ok) {
        header.magic = CV_FRAME_MAGIC;
        ok = pwrite(fd, &header, sizeof(header), 0) == ssize_t(sizeof(header));
    }
    flock(fd, LOCK_UN);
    perf_count(ok ? "cv_published" : "cv_publish_error");
    perf_observe("cv_publish", perf_now_us() - now);
}

std::string cv_status_json() {
    std::ifstream file("/tmp/webserver_cv.json");
    if (!file) return "{\"available\":false,\"reason\":\"worker_not_started\"}";
    std::string body;
    char chunk[1024];
    while (file && body.size() <= 14000) {
        file.read(chunk, sizeof(chunk)); body.append(chunk, file.gcount());
    }
    if (body.empty() || body.size() > 14000 || body.front() != '{')
        return "{\"available\":false,\"reason\":\"invalid_worker_status\"}";
    const std::string key = "\"completed_mono_us\":";
    const auto pos = body.find(key);
    const uint64_t completed = pos == std::string::npos ? 0 : strtoull(body.c_str()+pos+key.size(), NULL, 10);
    const uint64_t now = perf_now_us();
    const uint64_t age = completed && now >= completed ? (now-completed)/1000 : UINT64_MAX;
    char prefix[180];
    snprintf(prefix, sizeof(prefix), "{\"available\":%s,\"result_age_ms\":%llu,\"result\":",
             age < 2000 ? "true" : "false", static_cast<unsigned long long>(age));
    return std::string(prefix) + body + ",\"mode\":"+std::to_string(cv_mode.load())+"}";
}
