#include <stdint.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "flv_muxer.h"
#include "h264_parser.h"

namespace {

bool write_packet(std::ofstream& output, const std::shared_ptr<Packet>& packet) {
    if (!packet || !packet->data || packet->data->empty()) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(packet->data->data()),
                 static_cast<std::streamsize>(packet->data->size()));
    return output.good();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: " << argv[0]
                  << " input.h264 output.flv [fps]\n";
        return 1;
    }

    uint32_t fps = 25;
    if (argc == 4) {
        long value = std::strtol(argv[3], NULL, 10);
        if (value <= 0 || value > 240) {
            std::cerr << "invalid fps: " << argv[3] << "\n";
            return 1;
        }
        fps = static_cast<uint32_t>(value);
    }

    H264Parser parser(argv[1]);
    if (!parser.good()) {
        std::cerr << parser.error() << "\n";
        return 1;
    }

    std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "failed to open FLV output: " << argv[2] << "\n";
        return 1;
    }

    FlvMuxer muxer;
    H264AccessUnitAssembler assembler;
    if (!write_packet(output, muxer.make_flv_header())) {
        std::cerr << "failed to write FLV header\n";
        return 1;
    }

    uint64_t frame_index = 0;
    size_t nal_count = 0;
    size_t key_frame_count = 0;
    bool config_dirty = false;

    const auto write_access_unit =
        [&](H264AccessUnit& access_unit) -> bool {
            if (access_unit.empty()) {
                return true;
            }
            if (!muxer.ready()) {
                std::cerr << "video frame found before SPS/PPS\n";
                return false;
            }

            uint64_t timestamp = frame_index * 1000 / fps;
            access_unit.dts_ms = timestamp;
            access_unit.pts_ms = timestamp;

            if (config_dirty) {
                if (!write_packet(output, muxer.make_avc_sequence_header(
                        static_cast<uint32_t>(timestamp)))) {
                    return false;
                }
                config_dirty = false;
            }

            if (access_unit.is_key_frame()) {
                ++key_frame_count;
            }
            if (!write_packet(output, muxer.make_video_tag(access_unit))) {
                return false;
            }
            ++frame_index;
            return true;
        };

    H264Nal nal;
    while (parser.next_nal(nal)) {
        ++nal_count;

        if (nal.is_sps() || nal.is_pps()) {
            H264AccessUnit completed;
            if (assembler.flush(completed) && !write_access_unit(completed)) {
                return 1;
            }

            if (nal.is_sps()) {
                muxer.set_sps(nal.data);
            } else {
                muxer.set_pps(nal.data);
            }
            config_dirty = true;
            continue;
        }

        H264AccessUnit completed;
        if (assembler.push(nal, completed) && !write_access_unit(completed)) {
            return 1;
        }
    }

    H264AccessUnit completed;
    if (assembler.flush(completed) && !write_access_unit(completed)) {
        return 1;
    }

    output.close();
    if (!output) {
        std::cerr << "failed to finish FLV output\n";
        return 1;
    }

    std::cout << "converted " << nal_count << " NAL units into "
              << frame_index << " video frames, key_frames="
              << key_frame_count << ", fps=" << fps << "\n";
    return 0;
}
