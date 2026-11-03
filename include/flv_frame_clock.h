#pragma once
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
// AMF0 string event + JSON string. Prepend to the corresponding FLV video tag.
inline std::vector<uint8_t> flv_frame_clock(uint64_t capture_us,uint32_t pts_ms) {
    const char* name="onFrameClock"; char json[128];
    snprintf(json,sizeof(json),"{\"capture_us\":%llu,\"pts_ms\":%u}",
             (unsigned long long)capture_us,pts_ms);
    std::vector<uint8_t> payload;
    for(const char* s : {name,static_cast<const char*>(json)}) {
        size_t n=strlen(s);payload.push_back(2);payload.push_back(n>>8);payload.push_back(n);
        payload.insert(payload.end(),s,s+n);
    }
    size_t n=payload.size();
    std::vector<uint8_t> tag={18,uint8_t(n>>16),uint8_t(n>>8),uint8_t(n),
        uint8_t(pts_ms>>16),uint8_t(pts_ms>>8),uint8_t(pts_ms),uint8_t(pts_ms>>24),0,0,0};
    tag.insert(tag.end(),payload.begin(),payload.end()); n+=11;
    tag.insert(tag.end(),{uint8_t(n>>24),uint8_t(n>>16),uint8_t(n>>8),uint8_t(n)});
    return tag;
}
