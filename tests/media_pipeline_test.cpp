#include <iostream>
#include <vector>

#include "flv_muxer.h"
#include "h264_parser.h"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        return false;
    }
    return true;
}

H264Nal make_slice(uint8_t nal_header, uint8_t first_mb_bits) {
    H264Nal nal;
    nal.data.push_back(nal_header);
    nal.data.push_back(first_mb_bits);
    return nal;
}

} // namespace

int main() {
    bool ok = true;

    H264AccessUnitAssembler assembler;
    H264AccessUnit completed;

    // Exp-Golomb: first_mb_in_slice=0 编码为比特 "1"。
    H264Nal first_slice = make_slice(0x41, 0x80);
    // first_mb_in_slice=1 编码为比特 "010"，说明它仍属于当前画面。
    H264Nal second_slice = make_slice(0x41, 0x40);
    H264Nal next_idr = make_slice(0x65, 0x80);

    ok &= expect(!assembler.push(first_slice, completed),
                 "first slice must not finish an access unit");
    ok &= expect(!assembler.push(second_slice, completed),
                 "second slice of the same frame must stay in the access unit");
    ok &= expect(assembler.push(next_idr, completed),
                 "next first slice must finish the previous access unit");
    ok &= expect(completed.nals.size() == 2,
                 "completed access unit must contain both slices");
    ok &= expect(!completed.is_key_frame(),
                 "the first access unit should be a non-IDR frame");

    H264AccessUnit idr_access_unit;
    ok &= expect(assembler.flush(idr_access_unit),
                 "flush must return the pending IDR access unit");
    ok &= expect(idr_access_unit.is_key_frame(),
                 "the pending access unit should be an IDR frame");

    FlvMuxer muxer;
    muxer.set_sps(std::vector<uint8_t>{0x67, 0x42, 0x00, 0x1F});
    muxer.set_pps(std::vector<uint8_t>{0x68, 0xCE, 0x06, 0xE2});
    ok &= expect(muxer.ready(), "muxer must be ready after SPS and PPS");

    std::shared_ptr<Packet> header = muxer.make_flv_header();
    ok &= expect(header && header->size() == 13,
                 "FLV header must contain 9-byte header and PreviousTagSize0");
    ok &= expect((*header->data)[0] == 'F' &&
                 (*header->data)[1] == 'L' &&
                 (*header->data)[2] == 'V',
                 "FLV signature must be present");

    std::shared_ptr<Packet> sequence =
        muxer.make_avc_sequence_header(0);
    ok &= expect(sequence && sequence->is_config,
                 "AVC sequence header must be generated");

    idr_access_unit.pts_ms = 40;
    idr_access_unit.dts_ms = 40;
    std::shared_ptr<Packet> video = muxer.make_video_tag(idr_access_unit);
    ok &= expect(video && video->is_key_frame,
                 "IDR access unit must produce a key-frame FLV tag");

    if (!ok) {
        return 1;
    }

    std::cout << "media pipeline tests passed\n";
    return 0;
}
