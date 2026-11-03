#include "http_conn.h"
#include <cassert>
#include <iostream>

struct HttpConnTestAccess {
    static void flv(http_conn& c) { c.m_conn_type=http_conn::CONN_FLV; c.m_wait_flv_key_frame=true; }
    static size_t offset(http_conn& c) {return c.m_send_offset;}
    static size_t depth(http_conn& c) {return c.m_send_queue.size();}
    static std::shared_ptr<std::vector<uint8_t>> back(http_conn& c){return c.m_send_queue.back().data;}
    static void reset(http_conn& c){c.m_send_queue.clear();c.m_flv_origin_ms=UINT64_MAX;c.m_wait_flv_key_frame=true;}
};
std::shared_ptr<Packet> packet(size_t size,bool key) {
    auto p=std::make_shared<Packet>(std::vector<uint8_t>(size,0x55));
    p->is_key_frame=key; return p;
}
int main() {
    int pair[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    int capacity=1024;setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&capacity,sizeof(capacity));
    http_conn::m_epollfd=epoll_create1(0);assert(http_conn::m_epollfd>=0);
    http_conn c;sockaddr_in addr={};c.init(pair[0],addr);HttpConnTestAccess::flv(c);
    // The first accepted keyframe is the playback origin, not the source's
    // global timestamp; metadata and video stay together across partial sends.
    for(unsigned i=0;i<2;++i){
        auto frame=std::make_shared<Packet>(std::vector<uint8_t>(20,0));
        (*frame->data)[0]=9;frame->is_key_frame=i==0;
        frame->timestamp_ms=5000+i*50;frame->source_mono_us=9000000+i*50000;
        assert(c.enqueue_flv_packet(frame)==http_conn::PACKET_ENQUEUED);
        auto wire=HttpConnTestAccess::back(c);assert((*wire)[0]==18);
        unsigned length=((*wire)[1]<<16)|((*wire)[2]<<8)|(*wire)[3];
        size_t video=length+15;assert((*wire)[video]==9);
        unsigned clock_pts=((*wire)[4]<<16)|((*wire)[5]<<8)|(*wire)[6]|((*wire)[7]<<24);
        unsigned video_pts=((*wire)[video+4]<<16)|((*wire)[video+5]<<8)|(*wire)[video+6]|((*wire)[video+7]<<24);
        assert(clock_pts==i*50 && video_pts==clock_pts);
        std::string metadata(wire->begin()+11,wire->begin()+11+length);
        assert(metadata.find("onFrameClock")!=std::string::npos);
        assert(metadata.find(std::to_string(frame->source_mono_us))!=std::string::npos);
    }
    HttpConnTestAccess::reset(c);
    assert(c.enqueue_flv_packet(packet(100,false))==http_conn::PACKET_DROPPED);
    assert(HttpConnTestAccess::depth(c)==0);
    assert(c.enqueue_flv_packet(packet(128*1024,true))==http_conn::PACKET_ENQUEUED);
    assert(c.write()); // Forced short send followed by EAGAIN.
    size_t offset=HttpConnTestAccess::offset(c);assert(offset>0 && offset<128*1024);
    size_t depth=HttpConnTestAccess::depth(c);
    assert(c.enqueue_flv_packet(packet(512*1024,false))==http_conn::PACKET_DROPPED);
    assert(HttpConnTestAccess::offset(c)==offset);
    assert(HttpConnTestAccess::depth(c)==depth); // Never truncate a partial FLV tag.
    assert(!c.write()); // Main event loop must close and let player reconnect.
    c.close_conn();close(pair[1]);close(http_conn::m_epollfd);
    std::cout<<"send queue partial-write/IDR/backpressure tests passed\n";
}
