#ifndef FLV_MUXER_H
#define FLV_MUXER_H

#include <stdint.h>

#include <memory>
#include <vector>

#include "media_types.h"

class FlvMuxer {
public:
    void set_sps(const std::vector<uint8_t>& sps);
    void set_pps(const std::vector<uint8_t>& pps);

    bool ready() const;

    std::shared_ptr<Packet> make_flv_header() const;
    std::shared_ptr<Packet> make_avc_sequence_header(uint32_t timestamp_ms) const;
    std::shared_ptr<Packet> make_video_tag(const H264AccessUnit& access_unit) const;

private:
    static void write_u8(std::vector<uint8_t>& out, uint8_t value);
    static void write_be16(std::vector<uint8_t>& out, uint16_t value);
    static void write_be24(std::vector<uint8_t>& out, uint32_t value);
    static void write_be32(std::vector<uint8_t>& out, uint32_t value);
    static std::vector<uint8_t> make_flv_tag(
        uint8_t tag_type,
        uint32_t timestamp,
        const std::vector<uint8_t>& payload);

    std::vector<uint8_t> m_sps;
    std::vector<uint8_t> m_pps;
};

#endif
