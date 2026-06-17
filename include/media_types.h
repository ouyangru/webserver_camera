#ifndef MEDIA_TYPES_H
#define MEDIA_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <utility>
#include <vector>

struct Packet {
    std::shared_ptr<std::vector<uint8_t>> data;
    bool is_key_frame;
    bool is_config;
    uint64_t timestamp_ms;

    Packet()
        : is_key_frame(false), is_config(false), timestamp_ms(0) {}

    explicit Packet(std::vector<uint8_t>&& bytes)
        : data(std::make_shared<std::vector<uint8_t>>(std::move(bytes))),
          is_key_frame(false),
          is_config(false),
          timestamp_ms(0) {}

    size_t size() const {
        return data ? data->size() : 0;
    }
};

struct H264Nal {
    std::vector<uint8_t> data; // 不包含 Annex-B 起始码

    int type() const {
        return data.empty() ? -1 : (data[0] & 0x1F);
    }

    bool is_sps() const { return type() == 7; }
    bool is_pps() const { return type() == 8; }
    bool is_idr() const { return type() == 5; }
    bool is_vcl() const { return type() == 1 || type() == 5; }
};

struct H264AccessUnit {
    std::vector<H264Nal> nals;
    uint64_t pts_ms;
    uint64_t dts_ms;

    H264AccessUnit() : pts_ms(0), dts_ms(0) {}

    bool empty() const { return nals.empty(); }

    bool is_key_frame() const {
        for (size_t i = 0; i < nals.size(); ++i) {
            if (nals[i].is_idr()) {
                return true;
            }
        }
        return false;
    }
};

#endif
