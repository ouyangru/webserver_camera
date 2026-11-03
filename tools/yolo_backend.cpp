#include "yolo_backend.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <time.h>
static uint64_t clock_us(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000+t.tv_nsec/1000;}
const char* coco_label(int i){
 static const char* names[]={"person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow","elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket","bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple","sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair","couch","potted plant","bed","dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone","microwave","oven","toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear","hair drier","toothbrush"};
 return i>=0&&i<80?names[i]:"unknown";
}
void YoloBackend::close(){
 if(network) vip_finish_network(network);
 for(auto b:buffers) if(b) vip_destroy_buffer(b);
 buffers.clear();params.clear();
 if(network){vip_destroy_network(network);network=nullptr;}
 if(initialized){vip_destroy();initialized=false;}
}
bool YoloBackend::open(const char* model){
 close();
 if(vip_init(80*1024*1024)!=VIP_SUCCESS)return false;
 initialized=true;
 if(vip_create_network(model,0,VIP_CREATE_NETWORK_FROM_FILE,&network)!=VIP_SUCCESS)return false;
 uint32_t ni=0,no=0;
 if(vip_query_network(network,VIP_NETWORK_PROP_INPUT_COUNT,&ni)!=VIP_SUCCESS ||
    vip_query_network(network,VIP_NETWORK_PROP_OUTPUT_COUNT,&no)!=VIP_SUCCESS || ni!=1 || no!=3)return false;
 for(unsigned i=0;i<4;++i){
  vip_buffer_create_params_t p={};
  auto query=[&](vip_enum e,void* ptr){return i==0?vip_query_input(network,0,e,ptr):vip_query_output(network,i-1,e,ptr);};
  if(query(VIP_BUFFER_PROP_DATA_FORMAT,&p.data_format)!=VIP_SUCCESS ||
     query(VIP_BUFFER_PROP_NUM_OF_DIMENSION,&p.num_of_dims)!=VIP_SUCCESS ||
     query(VIP_BUFFER_PROP_SIZES_OF_DIMENSION,p.sizes)!=VIP_SUCCESS ||
     query(VIP_BUFFER_PROP_QUANT_FORMAT,&p.quant_format)!=VIP_SUCCESS)return false;
  if(p.quant_format==VIP_BUFFER_QUANTIZE_TF_ASYMM){
   if(query(VIP_BUFFER_PROP_TF_SCALE,&p.quant_data.affine.scale)!=VIP_SUCCESS ||
      query(VIP_BUFFER_PROP_TF_ZERO_POINT,&p.quant_data.affine.zeroPoint)!=VIP_SUCCESS)return false;
  }else if(p.quant_format!=VIP_BUFFER_QUANTIZE_NONE)return false;
  vip_buffer buffer=nullptr;
  if(vip_create_buffer(&p,sizeof(p),&buffer)!=VIP_SUCCESS)return false;
  buffers.push_back(buffer);params.push_back(p);
  fprintf(stderr,"yolo tensor=%u format=%d dims=%u shape=%u,%u,%u,%u bytes=%u scale=%g zp=%d\n",i,p.data_format,p.num_of_dims,p.sizes[0],p.sizes[1],p.sizes[2],p.sizes[3],vip_get_buffer_size(buffer),p.quant_data.affine.scale,p.quant_data.affine.zeroPoint);
 }
 if(vip_get_buffer_size(buffers[0])!=640*640*3 || params[0].data_format!=VIP_BUFFER_FORMAT_UINT8)return false;
 if(vip_prepare_network(network)!=VIP_SUCCESS)return false;
 for(unsigned i=0;i<4;++i)
  if((i==0?vip_set_input(network,0,buffers[i]):vip_set_output(network,i-1,buffers[i]))!=VIP_SUCCESS)return false;
 return true;
}
static float sigmoid(float x){return 1.f/(1.f+expf(-x));}
bool YoloBackend::run(const unsigned char* frame,unsigned w,unsigned h,int rotation,
 std::vector<Detection>& results,unsigned& rw,unsigned& rh,unsigned& ox,unsigned& oy,
 double& pre_ms,double& run_ms,double& post_ms){
 uint64_t begin=clock_us();
 auto rgb=static_cast<unsigned char*>(vip_map_buffer(buffers[0]));if(!rgb)return false;
 memset(rgb,114,640*640*3);
 unsigned iw=rotation%180?h:w,ih=rotation%180?w:h;
 double scale=std::min(640.0/iw,640.0/ih);
 rw=std::max(2u,unsigned(iw*scale)&~1u);rh=std::max(2u,unsigned(ih*scale)&~1u);
 ox=(640-rw)/2;oy=(640-rh)/2;
 auto clamp=[](int v){return std::max(0,std::min(255,v));};
 for(unsigned yy=0;yy<rh;++yy)for(unsigned x=0;x<rw;++x){
  unsigned rx=x*iw/rw,ry=yy*ih/rh,sx=rx,sy=ry;
  if(rotation==90){sx=ry;sy=h-1-rx;}if(rotation==180){sx=w-1-rx;sy=h-1-ry;}if(rotation==270){sx=w-1-ry;sy=rx;}
  int Y=std::max(0,int(frame[sy*w+sx])-16),U=int(frame[w*h+(sy/2)*w+(sx&~1u)])-128,V=int(frame[w*h+(sy/2)*w+(sx&~1u)+1])-128;
  size_t dst=(oy+yy)*640+ox+x;
  // Supplier demo uses planar B,G,R, not interleaved RGB.
  rgb[dst]=clamp((298*Y+516*U+128)>>8);
  rgb[640*640+dst]=clamp((298*Y-100*U-208*V+128)>>8);
  rgb[2*640*640+dst]=clamp((298*Y+409*V+128)>>8);
 }
 vip_flush_buffer(buffers[0],VIP_BUFFER_OPER_TYPE_FLUSH);
 uint64_t prepared=clock_us();
 if(vip_run_network(network)!=VIP_SUCCESS)return false;
 uint64_t ran=clock_us();
 static const float anchors[18]={10,13,16,30,33,23,30,61,62,45,59,119,116,90,156,198,373,326};
 std::vector<Detection> proposals;
 for(unsigned i=1;i<4;++i){
  auto p=params[i];size_t bytes=vip_get_buffer_size(buffers[i]);
  unsigned grid=0;for(unsigned g:{80u,40u,20u})if(bytes==size_t(g)*g*255)grid=g;
  if(!grid || p.data_format!=VIP_BUFFER_FORMAT_UINT8 || p.quant_format!=VIP_BUFFER_QUANTIZE_TF_ASYMM)return false;
  vip_flush_buffer(buffers[i],VIP_BUFFER_OPER_TYPE_INVALIDATE);
  const unsigned char* data=static_cast<unsigned char*>(vip_map_buffer(buffers[i]));if(!data)return false;
  const unsigned stride=640/grid,group=stride==8?0:stride==16?1:2;
  auto val=[&](size_t n){return (int(data[n])-p.quant_data.affine.zeroPoint)*p.quant_data.affine.scale;};
  for(unsigned a=0;a<3;++a)for(unsigned y=0;y<grid;++y)for(unsigned x=0;x<grid;++x){
   size_t n=(a*grid*grid+y*grid+x)*85;
   float object=sigmoid(val(n+4));if(object<0.35f)continue;
   int cls=0;float best=val(n+5);for(int c=1;c<80;++c)if(val(n+5+c)>best){best=val(n+5+c);cls=c;}
   float score=object*sigmoid(best);if(score<0.35f)continue;
   float cx=(sigmoid(val(n))*2-0.5f+x)*stride,cy=(sigmoid(val(n+1))*2-0.5f+y)*stride;
   float bw=powf(sigmoid(val(n+2))*2,2)*anchors[group*6+a*2],bh=powf(sigmoid(val(n+3))*2,2)*anchors[group*6+a*2+1];
   if(proposals.size()<2000)proposals.push_back({cx-bw/2,cy-bh/2,cx+bw/2,cy+bh/2,score,cls});
  }
 }
 std::sort(proposals.begin(),proposals.end(),[](const Detection&a,const Detection&b){return a.score>b.score;});
 results.clear();
 for(const auto& b:proposals){
  bool keep=true;for(const auto& a:results){if(a.label!=b.label)continue;
   float inter=std::max(0.f,std::min(a.x2,b.x2)-std::max(a.x1,b.x1))*std::max(0.f,std::min(a.y2,b.y2)-std::max(a.y1,b.y1));
   float total=(a.x2-a.x1)*(a.y2-a.y1)+(b.x2-b.x1)*(b.y2-b.y1)-inter;
   if(total>0 && inter/total>0.45f){keep=false;break;}}
  if(keep)results.push_back(b);
  if(results.size()>=30)break;
 }
 pre_ms=(prepared-begin)/1000.;run_ms=(ran-prepared)/1000.;post_ms=(clock_us()-ran)/1000.;
 return true;
}
