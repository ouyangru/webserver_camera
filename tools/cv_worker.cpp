#include "awnn.h"
#include "cv_frame_bus.h"
#include "yolo_backend.h"
#include <deque>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping = 0;
static void stop(int) { stopping = 1; }
static uint64_t now_us() {
    timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec)*1000000 + t.tv_nsec/1000;
}
static uint64_t cpu_us() {
    timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t);
    return uint64_t(t.tv_sec)*1000000+t.tv_nsec/1000;
}
static void publish(const std::string& text) {
    static std::deque<std::string> history;
    if(text.find("\"state\":\"running\"")!=std::string::npos) {
        history.push_back(text);while(history.size()>80)history.pop_front();
    } else history.clear();
    std::ofstream hf("/tmp/webserver_cv_history.json.next");
    hf << "{\"results\":[";
    for(size_t i=0;i<history.size();++i){if(i)hf<<',';hf<<history[i];}
    hf << "]}";hf.close();
    if(hf)rename("/tmp/webserver_cv_history.json.next","/tmp/webserver_cv_history.json");
    FILE* f = fopen("/tmp/webserver_cv.json.next", "w");
    if (!f) return;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (fclose(f)) ok = false;
    if (ok) rename("/tmp/webserver_cv.json.next", "/tmp/webserver_cv.json");
}
static unsigned rss_kb() {
    std::ifstream file("/proc/self/status"); std::string line;
    while (std::getline(file,line)) if (line.compare(0,6,"VmRSS:")==0)
        return strtoul(line.c_str()+6, NULL, 10);
    return 0;
}
// Preserve aspect ratio within 384x216, with even offsets for NV12 chroma.
// Model has 384x224 storage; final 8 rows are padding, as in supplier demo.
static void resize_nv12(const std::vector<unsigned char>& source, unsigned sw, unsigned sh,
                        std::vector<unsigned char>& y, std::vector<unsigned char>& uv,
                        unsigned& rw, unsigned& rh, unsigned& ox, unsigned& oy, int rotation) {
    const unsigned iw=rotation%180 ? sh : sw, ih=rotation%180 ? sw : sh;
    const double scale = std::min(384.0/iw, 216.0/ih);
    rw = std::max(2u, unsigned(iw*scale)&~1u); rh = std::max(2u,unsigned(ih*scale)&~1u);
    ox = ((384-rw)/2)&~1u; oy = ((216-rh)/2)&~1u;
    std::fill(y.begin(), y.end(), 16); std::fill(uv.begin(), uv.end(), 128);
    auto map = [=](unsigned x,unsigned yy,unsigned& sx,unsigned& sy) {
        if(rotation==90){sx=yy;sy=sh-1-x;}
        else if(rotation==180){sx=sw-1-x;sy=sh-1-yy;}
        else if(rotation==270){sx=sw-1-yy;sy=x;}
        else {sx=x;sy=yy;}
    };
    for (unsigned row=0; row<rh; ++row) for (unsigned col=0; col<rw; ++col) {
        unsigned sx,sy; map(col*iw/rw,row*ih/rh,sx,sy);
        y[(oy+row)*384+ox+col] = source[size_t(sy)*sw+sx];
    }
    const size_t base=size_t(sw)*sh;
    for (unsigned row=0; row<rh/2; ++row) for (unsigned col=0; col<rw; col+=2) {
        unsigned sx,sy; map(col*iw/rw,(row*2)*ih/rh,sx,sy);
        const size_t s=base+size_t(sy/2)*sw+(sx&~1u);
        const size_t d=size_t(oy/2+row)*384+ox+col;
        uv[d]=source[s]; uv[d+1]=source[s+1];
    }
}
int main(int argc, char** argv) {
    if (argc != 2 && argc != 5) { fprintf(stderr,"usage: webserver_cv MODEL.nb [INPUT.nv12 WIDTH HEIGHT]\n"); return 2; }
    const bool offline = argc == 5;
    const int rotation=getenv("CV_ROTATE") ? atoi(getenv("CV_ROTATE")) : 0;
    if(rotation!=0 && rotation!=90 && rotation!=180 && rotation!=270) return 2;
    CvFrameHeader fixture={};
    if (offline) {
        unsigned w=strtoul(argv[3],NULL,10), h=strtoul(argv[4],NULL,10);
        if (w<2 || h<2 || w>1920 || h>1080 || (w&1) || (h&1)) return 2;
        fixture={CV_FRAME_MAGIC,w,h,uint32_t(w*h*3/2),1,0};
    }
    signal(SIGTERM,stop); signal(SIGINT,stop);
    publish("{\"state\":\"loading\",\"completed_mono_us\":0,\"boxes\":[]}");
    awnn_info_t* info=awnn_get_info(argv[1]);
    if (!info || info->width!=384 || info->height!=224 || !info->mem_size || info->mem_size>64*1024*1024) {
        publish("{\"state\":\"invalid_model\",\"completed_mono_us\":0,\"boxes\":[]}"); return 3;
    }
    Awnn_Context_t* context=nullptr;
    YoloBackend yolo;
    unsigned active=0,loaded=0;
    bool disabled_reported=false;
    std::vector<unsigned char> source(CV_MAX_BYTES), y(384*224), uv(384*224/2);
    if (offline) {
        std::ifstream input(argv[2],std::ios::binary);
        input.read(reinterpret_cast<char*>(source.data()),fixture.bytes);
        if (input.gcount()!=fixture.bytes || input.peek()!=EOF) {
            fprintf(stderr,"input NV12 size mismatch\n");return 2;
        }
    }
    unsigned char* inputs[]={y.data(),uv.data()};
    int fd=-1; uint64_t last_stamp=0, count=0, begin=now_us(), next=0, last_report=0;
    uint64_t cpu_prev=cpu_us(), wall_prev=begin;
    FILE* log=offline ? stdout : fopen("/tmp/webserver_cv.log","a");
    publish("{\"state\":\"waiting_for_camera\",\"completed_mono_us\":0,\"boxes\":[]}");
    while (!stopping) {
        uint64_t start=now_us();
        unsigned mode=0;
        if(offline)mode=getenv("CV_MODEL") && std::string(getenv("CV_MODEL"))=="objects" ? 3:1;
        else {std::ifstream control("/tmp/webserver_cv.control");control>>mode;if(mode>3)mode=0;}
        if(mode!=active || (!loaded && mode)) {
            publish("{\"state\":\"loading\",\"completed_mono_us\":0,\"boxes\":[]}");
            unsigned desired=(mode&2)?2:mode?1:0;
            if(!desired || desired!=loaded){
                if(context){awnn_destroy(context);awnn_uninit();context=nullptr;}
                yolo.close();loaded=0;
                if(desired==1){awnn_init(info->mem_size);context=awnn_create(argv[1]);if(context)loaded=1;else awnn_uninit();}
                if(desired==2 && yolo.open("/usr/share/webserver_camera/models/yolov5_model.nb"))loaded=2;
            }
            active=mode;count=0;begin=now_us();next=0;last_stamp=0;
            if(mode && !loaded){publish("{\"state\":\"model_load_failed\",\"completed_mono_us\":0,\"boxes\":[]}");if(offline)return 4;usleep(1000000);continue;}
        }
        if(!mode){
            if(!disabled_reported)publish("{\"state\":\"disabled\",\"completed_mono_us\":0,\"boxes\":[]}");
            disabled_reported=true;
            usleep(200000);continue;
        }
        disabled_reported=false;
        start=now_us(); // Do not charge model loading to a camera frame's inference stages.
        if (start<next) { usleep(10000); continue; }
        if (!offline && fd<0) fd=open("/tmp/webserver_cv.frame",O_RDONLY|O_CLOEXEC);
        CvFrameHeader h={}; bool valid=false;
        if (fd>=0 && !flock(fd,LOCK_SH|LOCK_NB)) {
            valid=pread(fd,&h,sizeof(h),0)==ssize_t(sizeof(h)) && h.magic==CV_FRAME_MAGIC &&
                h.width>=2 && h.height>=2 && h.width<=1920 && h.height<=1080 &&
                !(h.width&1) && !(h.height&1) && h.bytes==size_t(h.width)*h.height*3/2 &&
                h.bytes<=CV_MAX_BYTES && h.mono_us!=last_stamp && h.mono_us<=start && start-h.mono_us<1000000;
            if (valid) valid=pread(fd,source.data(),h.bytes,sizeof(h))==ssize_t(h.bytes);
            flock(fd,LOCK_UN);
        }
        if (offline) { h=fixture; h.mono_us=start; valid=true; }
        if (!valid) { usleep(20000); continue; }
        last_stamp=h.mono_us; next=start+(loaded==1?40000:200000);
        unsigned rw,rh,ox,oy;
        std::vector<Detection> detections;
        uint64_t pre=0,ran=0;
        if(loaded==1){
            resize_nv12(source,h.width,h.height,y,uv,rw,rh,ox,oy,rotation);
            pre=now_us();awnn_set_input_buffers(context,inputs);awnn_run(context);ran=now_us();
            Awnn_Post_t post={AWNN_DET_POST_HUMANOID_3,384,216,0.35f};
            Awnn_Result_t result={};awnn_det_post(context,&post,&result);
            for(int i=0;i<std::min(100,std::max(0,result.valid_cnt));++i){const auto& b=result.boxes[i];
                if(b.label==0)detections.push_back({float(b.xmin),float(b.ymin),float(b.xmax),float(b.ymax),b.score,0});}
        } else {
            double a,b,c;
            const uint64_t infer_begin=now_us();
            if(!yolo.run(source.data(),h.width,h.height,rotation,detections,rw,rh,ox,oy,a,b,c)){
                publish("{\"state\":\"inference_error\",\"completed_mono_us\":0,\"boxes\":[]}");
                usleep(200000);continue;
            }
            pre=infer_begin+uint64_t(a*1000);ran=pre+uint64_t(b*1000);
        }
        uint64_t done=now_us(); ++count;
        const uint64_t cpu=cpu_us();
        const double cpu_pct=done>wall_prev ? 100.0*(cpu-cpu_prev)/(done-wall_prev) : 0;
        cpu_prev=cpu; wall_prev=done;
        std::ostringstream out;
        out << "{\"state\":\"running\",\"model\":\"" << (loaded==1?"100ASK_humandet_384_224_V1":"YOLOv5s-COCO80") << "\",\"backend\":\"NPU\","
            << "\"completed_mono_us\":" << done << ",\"frame_mono_us\":" << h.mono_us
            << ",\"frame_id\":" << h.sequence << ",\"source_width\":" << h.width
            << ",\"source_height\":" << h.height << ",\"frames\":" << count
            << ",\"rotation\":" << rotation
            << ",\"mode\":" << mode
            << ",\"fps\":" << (count*1000000.0/(done-begin)) << ",\"rss_kb\":" << rss_kb()
            << ",\"cpu_pct\":" << cpu_pct << ",\"npu_heap_bytes\":" << (loaded==1?info->mem_size:80*1024*1024)
            << ",\"preprocess_ms\":" << (pre-start)/1000.0 << ",\"run_ms\":" << (ran-pre)/1000.0
            << ",\"postprocess_ms\":" << (done-ran)/1000.0 << ",\"frame_age_ms\":" << (done-h.mono_us)/1000.0
            << ",\"boxes\":[";
        unsigned written=0;
        for (const auto& b:detections) {
            if(written>=8)break;
            if ((b.label==0 && !(mode&1)) || (b.label!=0 && !(mode&2)) || !(b.score>=0.35f && b.score<=1)) continue;
            const unsigned iw=rotation%180 ? h.height : h.width, ih=rotation%180 ? h.width : h.height;
            double left=(b.x1-int(ox))*double(iw)/rw, right=(b.x2-int(ox))*double(iw)/rw;
            double top=(b.y1-int(oy))*double(ih)/rh, bottom=(b.y2-int(oy))*double(ih)/rh;
            double ax=left,ay=top,bx=right,by=bottom;
            if(rotation==90){ax=top;ay=h.height-right;bx=bottom;by=h.height-left;}
            if(rotation==180){ax=h.width-right;ay=h.height-bottom;bx=h.width-left;by=h.height-top;}
            if(rotation==270){ax=h.width-bottom;ay=left;bx=h.width-top;by=right;}
            int x1=std::max(0,std::min(int(h.width),int(ax))),x2=std::max(0,std::min(int(h.width),int(bx)));
            int y1=std::max(0,std::min(int(h.height),int(ay))),y2=std::max(0,std::min(int(h.height),int(by)));
            if (x2<=x1 || y2<=y1) continue;
            if (written++) out << ',';
            out << "{\"label\":\"" << coco_label(b.label) << "\",\"class_id\":" << b.label << ",\"score\":" << b.score << ",\"x\":" << x1 << ",\"y\":" << y1
                << ",\"width\":" << x2-x1 << ",\"height\":" << y2-y1 << '}';
        }
        out << "]}"; publish(out.str());
        if (offline) { printf("RESULT_JSON=%s\n",out.str().c_str()); break; }
        if (done-last_report>5000000) {
            if(log && log!=stdout && ftell(log)>=1024*1024) {
                fclose(log); rename("/tmp/webserver_cv.log","/tmp/webserver_cv.log.1");
                log=fopen("/tmp/webserver_cv.log","w");
            }
            if(log) {
                fprintf(log,"mono_us=%llu cv frames=%llu run_ms=%.2f age_ms=%.2f persons=%u rss_kb=%u cpu_pct=%.2f\n",
                    (unsigned long long)done,(unsigned long long)count,(ran-pre)/1000.0,
                    (done-h.mono_us)/1000.0,written,rss_kb(),cpu_pct);
                fflush(log);
            }
            last_report=done;
        }
    }
    if(fd>=0) close(fd);
    if(log && log!=stdout) fclose(log);
    if(context){awnn_destroy(context);awnn_uninit();}
    yolo.close();
    publish("{\"state\":\"stopped\",\"completed_mono_us\":0,\"boxes\":[]}");
    return 0;
}
