#include "flv_muxer.h"

#include <stdexcept>
#include <utility>

void FlvMuxer::set_sps(const std::vector<uint8_t>& sps) {
    m_sps = sps;
}

void FlvMuxer::set_pps(const std::vector<uint8_t>& pps) {
    m_pps = pps;
}

bool FlvMuxer::ready() const {
    return m_sps.size() >= 4 && !m_pps.empty() &&
           m_sps.size() <= 0xFFFF && m_pps.size() <= 0xFFFF;
}

void FlvMuxer::write_u8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void FlvMuxer::write_be16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void FlvMuxer::write_be24(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void FlvMuxer::write_be32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

std::vector<uint8_t> FlvMuxer::make_flv_tag(
        uint8_t tag_type,
        uint32_t timestamp,
        const std::vector<uint8_t>& payload) {
    if (payload.size() > 0xFFFFFF) {
        throw std::runtime_error("FLV tag payload is too large");
    }

    std::vector<uint8_t> tag;
    tag.reserve(11 + payload.size() + 4);
    write_u8(tag, tag_type);
    write_be24(tag, static_cast<uint32_t>(payload.size()));
    write_be24(tag, timestamp & 0xFFFFFF);
    write_u8(tag, static_cast<uint8_t>((timestamp >> 24) & 0xFF));
    write_be24(tag, 0);
    tag.insert(tag.end(), payload.begin(), payload.end());
    write_be32(tag, static_cast<uint32_t>(11 + payload.size()));
    return tag;
}

std::shared_ptr<Packet> FlvMuxer::make_flv_header() const {
    std::vector<uint8_t> out;
    out.reserve(13);
    out.push_back('F');
    out.push_back('L');
    out.push_back('V');
    out.push_back(0x01);
    out.push_back(0x01); // video only
    write_be32(out, 9);
    write_be32(out, 0); // PreviousTagSize0

    std::shared_ptr<Packet> packet =
        std::make_shared<Packet>(std::move(out));
    packet->is_config = true;
    return packet;
}

std::shared_ptr<Packet> FlvMuxer::make_avc_sequence_header(
        uint32_t timestamp_ms) const {
    if (!ready()) {
        return std::shared_ptr<Packet>();
    }

    std::vector<uint8_t> payload;
    payload.reserve(16 + m_sps.size() + m_pps.size());
    write_u8(payload, 0x17); // key frame + AVC
    write_u8(payload, 0x00); // AVC sequence header
    write_be24(payload, 0);

    write_u8(payload, 0x01);     // configurationVersion
    write_u8(payload, m_sps[1]); // AVCProfileIndication
    write_u8(payload, m_sps[2]); // profile_compatibility
    write_u8(payload, m_sps[3]); // AVCLevelIndication
    write_u8(payload, 0xFF);     // lengthSizeMinusOne = 3
    write_u8(payload, 0xE1);     // one SPS
    write_be16(payload, static_cast<uint16_t>(m_sps.size()));
    payload.insert(payload.end(), m_sps.begin(), m_sps.end());
    write_u8(payload, 0x01);     // one PPS
    write_be16(payload, static_cast<uint16_t>(m_pps.size()));
    payload.insert(payload.end(), m_pps.begin(), m_pps.end());

    std::shared_ptr<Packet> packet = std::make_shared<Packet>(
        make_flv_tag(0x09, timestamp_ms, payload));
    packet->is_config = true;
    packet->is_key_frame = true;
    packet->timestamp_ms = timestamp_ms;
    return packet;
}

std::shared_ptr<Packet> FlvMuxer::make_video_tag(
        const H264AccessUnit& access_unit) const {
    if (access_unit.empty()) {
        return std::shared_ptr<Packet>();
    }

    bool key_frame = access_unit.is_key_frame();
    std::vector<uint8_t> payload;
    payload.push_back(key_frame ? 0x17 : 0x27);
    payload.push_back(0x01); // AVC NALU

    int64_t composition_time =
        static_cast<int64_t>(access_unit.pts_ms) -
        static_cast<int64_t>(access_unit.dts_ms);
    write_be24(payload, static_cast<uint32_t>(composition_time) & 0xFFFFFF);

    for (size_t i = 0; i < access_unit.nals.size(); ++i) {
        const H264Nal& nal = access_unit.nals[i];
        if (nal.data.empty() || nal.is_sps() || nal.is_pps() || nal.type() == 9) {
            continue;
        }
        if (nal.data.size() > 0xFFFFFFFFu) {
            throw std::runtime_error("H264 NAL is too large");
        }
        write_be32(payload, static_cast<uint32_t>(nal.data.size()));
        payload.insert(payload.end(), nal.data.begin(), nal.data.end());
    }

    std::shared_ptr<Packet> packet = std::make_shared<Packet>(
        make_flv_tag(0x09, static_cast<uint32_t>(access_unit.dts_ms), payload));
    packet->is_key_frame = key_frame;
    packet->timestamp_ms = access_unit.dts_ms;
    return packet;
}
