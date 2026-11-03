#include "h264_encoder.h"
#include "pipeline_metrics.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <unistd.h>
#if ENABLE_CEDAR
#include <vencoder.h>
#include <memoryAdapter.h>
#endif

namespace {
struct EncodedBus {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::vector<H264Nal>> frames;
    std::deque<uint64_t> times;
    std::string error;
    bool active = false;
    bool need_key = true;
} bus;

std::vector<H264Nal> annex_b_nals(const std::vector<unsigned char>& bytes) {
    std::vector<H264Nal> result;
    size_t start = 0;
    bool found = false;
    for (size_t i = 0; i + 2 < bytes.size();) {
        size_t prefix = 0;
        if (bytes[i] == 0 && bytes[i+1] == 0) {
            if (bytes[i+2] == 1) prefix = 3;
            else if (i+3 < bytes.size() && bytes[i+2] == 0 && bytes[i+3] == 1) prefix = 4;
        }
        if (!prefix) { ++i; continue; }
        if (found && i > start) {
            H264Nal n; n.data.assign(bytes.begin()+start, bytes.begin()+i); result.push_back(n);
        }
        start = i + prefix; found = true; i = start;
    }
    if (found && start < bytes.size()) {
        H264Nal n; n.data.assign(bytes.begin()+start, bytes.end()); result.push_back(n);
    }
    return result;
}

void append_cedar_nals(std::vector<H264Nal>& out,
                       const unsigned char* data, size_t size) {
    if (!data || !size) return;
    std::vector<unsigned char> bytes(data, data + size);
    std::vector<H264Nal> parsed = annex_b_nals(bytes);
    if (!parsed.empty()) {
        out.insert(out.end(), parsed.begin(), parsed.end());
        return;
    }

    // Cedar releases differ here: some return Annex-B, some AVCC, and some
    // expose one raw NAL in each pData segment. Normalize all three forms.
    size_t offset = 0;
    bool avcc = false;
    while (offset + 4 <= size) {
        size_t length = (size_t(data[offset]) << 24) |
                        (size_t(data[offset + 1]) << 16) |
                        (size_t(data[offset + 2]) << 8) | data[offset + 3];
        if (!length || offset + 4 + length > size) break;
        H264Nal nal;
        nal.data.assign(data + offset + 4, data + offset + 4 + length);
        out.push_back(std::move(nal));
        offset += 4 + length;
        avcc = true;
    }
    if (avcc && offset == size) return;

    unsigned type = data[0] & 0x1f;
    if (type > 0 && type < 24) {
        H264Nal nal;
        nal.data.assign(data, data + size);
        out.push_back(std::move(nal));
    }
}
class LiveSource : public IH264Source {
    std::deque<H264Nal> pending;
    std::string last_error;
public:
    LiveSource() {
        std::lock_guard<std::mutex> guard(bus.mutex);
        bus.frames.clear(); bus.times.clear(); bus.need_key = true;
        perf_encoder_queue(0,0);
    }
    bool good() const { return true; }
    const std::string& error() const { return last_error; }
    bool next_nal(H264Nal& nal) {
        if (pending.empty()) {
            std::unique_lock<std::mutex> guard(bus.mutex);
            if (!bus.changed.wait_for(guard, std::chrono::seconds(2), [] { return !bus.frames.empty(); })) {
                last_error = bus.error.empty() ? "waiting for hardware H264 frames" : bus.error;
                return false;
            }
            for (auto& n : bus.frames.front()) pending.push_back(std::move(n));
            perf_observe("encoder_queue_wait", perf_now_us()-bus.times.front());
            bus.times.pop_front();
            bus.frames.pop_front();
            perf_encoder_queue(bus.frames.size(),bus.times.empty()?0:bus.times.front());
        }
        nal = std::move(pending.front()); pending.pop_front(); return true;
    }
};
}

struct H264Encoder::Impl {
    std::string error;
    int width, height, fps;
    unsigned long count = 0;
    FILE* dump = NULL;
    unsigned dump_limit = 150;
#if ENABLE_CEDAR
    VideoEncoder* encoder = NULL;
    ScMemOpsS* memops = NULL;
    bool opened = false, initialized = false, allocated = false;
    VencInputBuffer input = {};
    std::mutex input_mutex;
    std::condition_variable input_changed;
    bool input_done = true;
    int input_result = 0;
    VencCbType callbacks = {};
    static int event(VideoEncoder*, void*, VencEventType, unsigned, unsigned, void*) { return 0; }
    static int done(VideoEncoder*, void* opaque, VencCbInputBufferDoneInfo* info) {
        Impl* self = static_cast<Impl*>(opaque);
        if (self->count < 3) fprintf(stderr, "[Cedar] input callback result=%d\n", info->nResult);
        {
            std::lock_guard<std::mutex> guard(self->input_mutex);
            self->input_result = info->nResult; self->input_done = true;
        }
        self->input_changed.notify_all(); return 0;
    }
    std::vector<unsigned char> header;
#endif
};

H264Encoder::H264Encoder(int width, int height, int fps, int bitrate, const char* path) : p(new Impl) {
    p->width = width; p->height = height; p->fps = fps;
    if (width <= 0 || height <= 0 || width % 16 || height % 16 || fps <= 0) {
        p->error = "Cedar NV12 dimensions must be positive multiples of 16"; return;
    }
#if ENABLE_CEDAR
    p->memops = MemAdapterGetOpsS();
    if (!p->memops || CdcMemOpen(p->memops) < 0) { p->error = "Cedar memory adapter open failed"; return; }
    p->opened = true;
    p->encoder = VencCreate(VENC_CODEC_H264);
    if (!p->encoder) { p->error = "VencCreate(H264) failed"; return; }
    VencH264Param param = {};
    param.sProfileLevel.nProfile = VENC_H264ProfileBaseline;
    param.sProfileLevel.nLevel = VENC_H264Level31;
    param.sQPRange.nMinqp = 20; param.sQPRange.nMaxqp = 45;
    param.nFramerate = fps; param.nSrcFramerate = fps;
    param.nBitrate = bitrate; param.nMaxKeyInterval = fps;
    param.nCodingMode = VENC_FRAME_CODING;
    if (VencSetParameter(p->encoder, VENC_IndexParamH264Param, &param) != 0) {
        p->error = "setting Cedar H264 parameters failed"; return;
    }
    unsigned vbv_bytes = 4 * 1024 * 1024;
    VencSetParameter(p->encoder, VENC_IndexParamSetVbvSize, &vbv_bytes);
    VencBaseConfig config = {};
    config.nInputWidth = config.nDstWidth = width;
    config.nInputHeight = config.nDstHeight = height;
    config.nStride = width; config.eInputFormat = VENC_PIXEL_YUV420SP;
    // V853's official MPP uses online mode with the online channel disabled
    // even for ordinary memory-fed encoding. Leaving bOnlineMode at zero
    // selects a legacy path that does not complete correctly on IC 0x23010.
    config.bOnlineMode = 1;
    config.bOnlineChannel = 0;
    config.bIsVbvNoCache = 1;
    config.rec_lbc_mode = LBC_MODE_1_5X;
    config.memops = p->memops;
    if (VencInit(p->encoder, &config) != 0) { p->error = "VencInit failed"; return; }
    p->initialized = true;
    VencAllocateBufferParam buffers = {};
    buffers.nSizeY = width * height; buffers.nSizeC = width * height / 2;
    if (VencAllocateInputBuf(p->encoder, &buffers, &p->input) != 0) { p->error = "Cedar input allocation failed"; return; }
    p->allocated = true;
    VencHeaderData header = {};
    if (VencGetParameter(p->encoder, VENC_IndexParamH264SPSPPS, &header) != 0 ||
        !header.pBuffer || !header.nLength) { p->error = "Cedar SPS/PPS missing"; return; }
    p->header.assign(header.pBuffer, header.pBuffer + header.nLength);
    p->callbacks.EventHandler = NULL; p->callbacks.InputBufferDone = Impl::done;
    if (VencSetCallbacks(p->encoder, &p->callbacks, p) != 0 || VencStart(p->encoder) != 0) {
        p->error = "VencStart/callback registration failed"; return;
    }
    if (path && *path) {
        p->dump = fopen(path, "wb");
        if (!p->dump) perror("[Cedar] H264 dump");
        const char* limit = getenv("H264_DUMP_FRAMES");
        if (limit && atoi(limit) > 0) p->dump_limit = atoi(limit);
    }
    {
        std::lock_guard<std::mutex> guard(bus.mutex);
        bus.active = true; bus.error.clear(); bus.need_key = true;
    }
    printf("[Cedar] hardware H264 ready: %dx%d %dfps bitrate=%d SPS/PPS=%zu\n",
        width, height, fps, bitrate, p->header.size());
#else
    (void)bitrate; (void)path;
    p->error = "built without ENABLE_CEDAR=1";
#endif
}
bool H264Encoder::good() const { return p->error.empty(); }
const std::string& H264Encoder::error() const { return p->error; }
bool H264Encoder::encode_frame(const unsigned char* yuv, uint64_t capture_mono_us) {
    const uint64_t frame_input_us = capture_mono_us ? capture_mono_us : perf_now_us();
    if (!good() || !yuv) return false;
#if ENABLE_CEDAR
    VencInputBuffer& input = p->input;
    input.bNeedFlushCache = 1;
    memcpy(input.pAddrVirY, yuv, size_t(p->width) * p->height);
    memcpy(input.pAddrVirC, yuv + size_t(p->width) * p->height, size_t(p->width) * p->height / 2);
    input.nPts = frame_input_us;
    CdcMemFlushCache(p->memops, input.pAddrVirY, p->width * p->height);
    CdcMemFlushCache(p->memops, input.pAddrVirC, p->width * p->height / 2);
    { std::lock_guard<std::mutex> guard(p->input_mutex); p->input_done = false; }
    if (VencQueueInputBuf(p->encoder, &input) != 0) { p->error = "VencQueueInputBuf failed"; return false; }
    {
        std::unique_lock<std::mutex> guard(p->input_mutex);
        if (!p->input_changed.wait_for(guard, std::chrono::seconds(2), [&] { return p->input_done; })) {
            p->error = "Cedar input completion timeout"; return false;
        }
        if (p->input_result != 0) { p->error = "Cedar hardware rejected input frame"; return false; }
    }
    VencOutputBuffer output = {};
    int tries = 0;
    while (VencDequeueOutputBuf(p->encoder, &output) != 0) {
        if (++tries >= 500) { p->error = "Cedar output timeout"; return false; }
        usleep(1000);
    }
    // The bitstream buffer is DMA-written by VE. The older Tina 4.9 memory
    // adapter does not invalidate it on dequeue, so synchronize it explicitly
    // before the CPU parses or copies the bytes.
    if (output.pData0 && output.nSize0) CdcMemFlushCache(p->memops, output.pData0, output.nSize0);
    if (output.pData1 && output.nSize1) CdcMemFlushCache(p->memops, output.pData1, output.nSize1);
    if (output.pData2 && output.nSize2) CdcMemFlushCache(p->memops, output.pData2, output.nSize2);
    if (p->count < 3) {
        fprintf(stderr, "[Cedar] output flag=%u sizes=%u/%u/%u data0=",
            output.nFlag, output.nSize0, output.nSize1, output.nSize2);
        for (unsigned i = 0; output.pData0 && i < output.nSize0 && i < 16; ++i)
            fprintf(stderr, "%02x", output.pData0[i]);
        fputc('\n', stderr);
    }
    std::vector<H264Nal> frame_nals;
    append_cedar_nals(frame_nals, output.pData0, output.nSize0);
    append_cedar_nals(frame_nals, output.pData1, output.nSize1);
    append_cedar_nals(frame_nals, output.pData2, output.nSize2);
    bool key = (output.nFlag & VENC_BUFFERFLAG_KEYFRAME) != 0;
    for (const auto& nal : frame_nals)
        if (!nal.data.empty() && (nal.data[0] & 0x1f) == 5) key = true;

    std::vector<H264Nal> nals;
    if (key) append_cedar_nals(nals, p->header.data(), p->header.size());
    nals.insert(nals.end(), frame_nals.begin(), frame_nals.end());
    VencQueueOutputBuf(p->encoder, &output);
    if (nals.empty()) {
        fprintf(stderr, "[Cedar] empty/unknown output: flag=%u sizes=%u/%u/%u\n",
            output.nFlag, output.nSize0, output.nSize1, output.nSize2);
        p->error = "encoder returned no recognizable H264 NALs"; return false;
    }
    if (p->dump) {
        static const unsigned char start_code[] = {0, 0, 0, 1};
        bool write_ok = true;
        for (const auto& nal : nals) {
            write_ok = fwrite(start_code, 1, sizeof(start_code), p->dump) == sizeof(start_code) &&
                       fwrite(nal.data.data(), 1, nal.data.size(), p->dump) == nal.data.size();
            if (!write_ok) break;
        }
        if (!write_ok) { perror("[Cedar] H264 write"); fclose(p->dump); p->dump = NULL; }
        else fflush(p->dump);
    }
    ++p->count;
    perf_count("encoded_frames");
    for (auto& n : nals) n.source_mono_us = frame_input_us;
    // The encoder already supplies complete frame boundaries. An AUD lets
    // the generic assembler flush immediately instead of waiting another frame.
    H264Nal aud; aud.data = {0x09, 0xf0}; aud.source_mono_us = frame_input_us;
    nals.push_back(std::move(aud));
    if (p->dump && p->count >= p->dump_limit) { fclose(p->dump); p->dump = NULL; }
    {
        std::lock_guard<std::mutex> guard(bus.mutex);
        const uint64_t now = perf_now_us();
        if (bus.frames.size() >= 4 || (!bus.times.empty() && now-bus.times.front()>250000)) {
            perf_count("encoder_queue_dropped", bus.frames.size());
            bus.frames.clear(); bus.times.clear(); bus.need_key = true;
        }
        if (key) bus.need_key = false;
        if (!bus.need_key) { bus.frames.push_back(std::move(nals)); bus.times.push_back(now); }
        else perf_count("encoder_wait_key_dropped");
        perf_encoder_queue(bus.frames.size(),bus.times.empty()?0:bus.times.front());
    }
    bus.changed.notify_all();
    if (p->count <= 5 || p->count % 150 == 0)
        printf("[Cedar] encoded frame=%lu bytes=%u key=%d nals=%zu flag=%u\n",
            p->count, output.nSize0 + output.nSize1 + output.nSize2,
            key, nals.size(), output.nFlag);
    return true;
#else
    return false;
#endif
}
void H264Encoder::flush() { if (p->dump) fflush(p->dump); }
H264Encoder::~H264Encoder() {
    {
        std::lock_guard<std::mutex> guard(bus.mutex);
        bus.active = false; bus.error = p->error.empty() ? "capture stopped" : p->error;
        bus.frames.clear(); bus.times.clear(); bus.need_key = true;
        perf_encoder_queue(0,0);
    }
    bus.changed.notify_all();
#if ENABLE_CEDAR
    if (p->encoder) {
        if (p->initialized) VencPause(p->encoder);
        VencDestroy(p->encoder);
    }
    if (p->opened) CdcMemClose(p->memops);
#endif
    if (p->dump) fclose(p->dump);
    delete p;
}
std::unique_ptr<IH264Source> create_cedar_h264_source() {
    return std::unique_ptr<IH264Source>(new LiveSource);
}
