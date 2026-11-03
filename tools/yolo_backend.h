#pragma once
#include <vip_lite.h>
#include <vector>
#include <string>
struct Detection { float x1,y1,x2,y2,score; int label; };
class YoloBackend {
    bool initialized=false;
    vip_network network=nullptr;
    std::vector<vip_buffer> buffers;
    std::vector<vip_buffer_create_params_t> params;
public:
    ~YoloBackend(){close();}
    void close();
    bool open(const char* model);
    bool run(const unsigned char* nv12,unsigned w,unsigned h,int rotation,
             std::vector<Detection>& results,unsigned& rw,unsigned& rh,unsigned& ox,unsigned& oy,
             double& pre_ms,double& run_ms,double& post_ms);
};
const char* coco_label(int id);
