#include "pipeline_metrics.h"
#include "logger.h"
#include <algorithm>
#include <deque>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <ctime>

namespace {
struct Queue { size_t depth=0, bytes=0; uint64_t oldest=0; };
struct State {
    std::mutex lock;
    std::map<std::string, std::deque<uint64_t>> stages;
    std::map<std::string,uint64_t> counts;
    std::map<int,Queue> send;
    Queue encoder;
    std::string snapshot="{}";
};
State& state() { static State* s=new State; return *s; }
uint64_t cpu_us() { timespec t={}; clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t); return uint64_t(t.tv_sec)*1000000+t.tv_nsec/1000; }
uint64_t field_kb(const char* path,const char* name) {
    std::ifstream f(path); std::string line;
    while(std::getline(f,line)) if(line.compare(0,std::char_traits<char>::length(name),name)==0) {
        std::istringstream v(line.substr(std::char_traits<char>::length(name))); uint64_t n=0; v>>n; return n;
    }
    return 0;
}
void collect() {
    uint64_t prev_wall=perf_now_us(),prev_cpu=cpu_us(),prev_frames=0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        uint64_t wall=perf_now_us(),cpu=cpu_us();
        double elapsed=double(wall-prev_wall)/1000000.0;
        double usage=100.0*double(cpu-prev_cpu)/double(wall-prev_wall);
        auto& s=state(); std::lock_guard<std::mutex> guard(s.lock);
        uint64_t frames=s.counts["encoded_frames"];
        double fps=double(frames-prev_frames)/elapsed;
        std::ostringstream out; out.setf(std::ios::fixed); out.precision(2);
        out<<"{\"sample_mono_us\":"<<wall<<",\"cpu_pct\":"<<usage
           <<",\"rss_kb\":"<<field_kb("/proc/self/status","VmRSS:")
           <<",\"available_kb\":"<<field_kb("/proc/meminfo","MemAvailable:")
           <<",\"encoded_fps\":"<<fps<<",\"counters\":{";
        bool first=true;
        for(auto& c:s.counts) { if(!first)out<<",";first=false;out<<"\""<<c.first<<"\":"<<c.second; }
        out<<"},\"stages_ms\":{"; first=true;
        for(auto& item:s.stages) {
            if(item.second.empty())continue;
            std::vector<uint64_t> sorted(item.second.begin(),item.second.end());
            std::sort(sorted.begin(),sorted.end()); double sum=0;
            for(auto n:sorted)sum+=n;
            if(!first)out<<",";first=false;
            out<<"\""<<item.first<<"\":{\"samples\":"<<sorted.size()
               <<",\"avg\":"<<sum/sorted.size()/1000.0
               <<",\"p95\":"<<sorted[(sorted.size()*95+99)/100-1]/1000.0
               <<",\"max\":"<<sorted.back()/1000.0<<"}";
        }
        size_t depth=0,bytes=0;uint64_t oldest=0;
        for(auto& q:s.send) { depth+=q.second.depth;bytes+=q.second.bytes;
            if(q.second.oldest && (!oldest||q.second.oldest<oldest))oldest=q.second.oldest; }
        out<<"},\"queues\":{\"encoder_frames\":"<<s.encoder.depth
           <<",\"encoder_oldest_ms\":"<<(s.encoder.oldest && wall>s.encoder.oldest?(wall-s.encoder.oldest)/1000.0:0)
           <<",\"send_packets\":"<<depth<<",\"send_bytes\":"<<bytes
           <<",\"send_oldest_ms\":"<<(oldest && wall>oldest?(wall-oldest)/1000.0:0)<<"}}";
        s.snapshot=out.str();
        LOG_INFO("metrics","event=summary data=%s",s.snapshot.c_str());
        prev_frames=frames;prev_wall=wall;prev_cpu=cpu;
    }
}
}
uint64_t perf_now_us(){return stream_log::monotonic_us();}
void perf_observe(const char* name,uint64_t elapsed){auto& s=state();std::lock_guard<std::mutex> g(s.lock);auto& q=s.stages[name];if(q.size()>=256)q.pop_front();q.push_back(elapsed);}
void perf_count(const char* name,uint64_t delta){auto& s=state();std::lock_guard<std::mutex> g(s.lock);s.counts[name]+=delta;}
void perf_encoder_queue(size_t depth,uint64_t oldest){auto& s=state();std::lock_guard<std::mutex> g(s.lock);s.encoder.depth=depth;s.encoder.oldest=oldest;}
void perf_send_queue(int fd,size_t depth,size_t bytes,uint64_t oldest){auto& s=state();std::lock_guard<std::mutex> g(s.lock);if(!depth)s.send.erase(fd);else{auto& q=s.send[fd];q.depth=depth;q.bytes=bytes;q.oldest=oldest;}}
std::string perf_json(){auto& s=state();std::lock_guard<std::mutex> g(s.lock);return s.snapshot;}
void perf_start(){static std::once_flag once;std::call_once(once,[]{std::thread(collect).detach();});}
