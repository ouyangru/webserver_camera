#include "h264_source.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>

namespace {

bool starts_with(const std::string& value, const char* prefix) {
    size_t len = strlen(prefix);
    return value.compare(0, len, prefix) == 0;
}

int xioctl(int fd, unsigned long request, void* arg) {
    int ret = 0;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

} // namespace

H264FileSource::H264FileSource(const std::string& filename)
    : m_parser(filename) {}

bool H264FileSource::good() const {
    return m_parser.good();
}

const std::string& H264FileSource::error() const {
    return m_parser.error();
}

bool H264FileSource::next_nal(H264Nal& nal) {
    return m_parser.next_nal(nal);
}

V4L2H264Source::V4L2H264Source(const V4L2H264SourceConfig& config)
    : m_config(config),
      m_fd(-1),
      m_streaming(false),
      m_pending_pos(0) {
    if (m_config.buffer_count == 0) {
        m_config.buffer_count = 4;
    }
    if (m_config.fps == 0) {
        m_config.fps = 25;
    }
    init_device();
}

V4L2H264Source::~V4L2H264Source() {
    close_device();
}

bool V4L2H264Source::good() const {
    return m_error.empty();
}

const std::string& V4L2H264Source::error() const {
    return m_error;
}

bool V4L2H264Source::init_device() {
    m_fd = open(m_config.device.c_str(), O_RDWR);
    if (m_fd < 0) {
        m_error = "failed to open V4L2 H264 device: " + m_config.device +
                  ": " + strerror(errno);
        return false;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        m_error = "VIDIOC_QUERYCAP failed on " + m_config.device +
                  ": " + strerror(errno);
        close_device();
        return false;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        m_error = m_config.device +
                  " does not expose VIDEO_CAPTURE + STREAMING H264 output";
        close_device();
        return false;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = m_config.width;
    fmt.fmt.pix.height = m_config.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
        m_error = "VIDIOC_S_FMT H264 failed on " + m_config.device +
                  ": " + strerror(errno);
        close_device();
        return false;
    }

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = m_config.fps;
    xioctl(m_fd, VIDIOC_S_PARM, &parm);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = m_config.buffer_count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(m_fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        m_error = "VIDIOC_REQBUFS failed on " + m_config.device +
                  ": " + strerror(errno);
        close_device();
        return false;
    }

    m_buffers.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            m_error = "VIDIOC_QUERYBUF failed on " + m_config.device +
                      ": " + strerror(errno);
            close_device();
            return false;
        }

        void* start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, m_fd, buf.m.offset);
        if (start == MAP_FAILED) {
            m_error = "mmap failed on " + m_config.device +
                      ": " + strerror(errno);
            close_device();
            return false;
        }

        m_buffers[i].start = start;
        m_buffers[i].length = buf.length;

        if (xioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
            m_error = "VIDIOC_QBUF failed on " + m_config.device +
                      ": " + strerror(errno);
            close_device();
            return false;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
        m_error = "VIDIOC_STREAMON failed on " + m_config.device +
                  ": " + strerror(errno);
        close_device();
        return false;
    }
    m_streaming = true;
    return true;
}

void V4L2H264Source::close_device() {
    if (m_fd >= 0 && m_streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(m_fd, VIDIOC_STREAMOFF, &type);
        m_streaming = false;
    }

    for (size_t i = 0; i < m_buffers.size(); ++i) {
        if (m_buffers[i].start && m_buffers[i].start != MAP_FAILED) {
            munmap(m_buffers[i].start, m_buffers[i].length);
            m_buffers[i].start = NULL;
            m_buffers[i].length = 0;
        }
    }
    m_buffers.clear();

    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

bool V4L2H264Source::find_start_code(size_t from,
                                     size_t& position,
                                     size_t& length) const {
    for (size_t i = from; i + 3 <= m_pending.size(); ++i) {
        if (m_pending[i] != 0x00 || m_pending[i + 1] != 0x00) {
            continue;
        }
        if (m_pending[i + 2] == 0x01) {
            position = i;
            length = 3;
            return true;
        }
        if (i + 4 <= m_pending.size() &&
            m_pending[i + 2] == 0x00 &&
            m_pending[i + 3] == 0x01) {
            position = i;
            length = 4;
            return true;
        }
    }
    return false;
}

bool V4L2H264Source::parse_pending_nal(H264Nal& nal) {
    nal.data.clear();
    if (m_pending_pos >= m_pending.size()) {
        return false;
    }

    size_t start = 0;
    size_t start_len = 0;
    if (!find_start_code(m_pending_pos, start, start_len)) {
        if (m_pending_pos == 0 && !m_pending.empty()) {
            nal.data.swap(m_pending);
            m_pending_pos = 0;
            return true;
        }
        m_pending.clear();
        m_pending_pos = 0;
        return false;
    }

    size_t nal_start = start + start_len;
    size_t next = 0;
    size_t next_len = 0;
    bool has_next = find_start_code(nal_start, next, next_len);
    size_t nal_end = has_next ? next : m_pending.size();
    while (nal_end > nal_start && m_pending[nal_end - 1] == 0x00) {
        --nal_end;
    }

    if (nal_end <= nal_start) {
        m_pending_pos = has_next ? next : m_pending.size();
        return parse_pending_nal(nal);
    }

    nal.data.assign(m_pending.begin() + nal_start, m_pending.begin() + nal_end);
    m_pending_pos = has_next ? next : m_pending.size();
    if (m_pending_pos >= m_pending.size()) {
        m_pending.clear();
        m_pending_pos = 0;
    }
    return !nal.data.empty();
}

bool V4L2H264Source::dequeue_encoded_buffer() {
    if (m_fd < 0 || !m_streaming) {
        return false;
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) {
        m_error = "VIDIOC_DQBUF failed on " + m_config.device +
                  ": " + strerror(errno);
        return false;
    }

    if (buf.index >= m_buffers.size()) {
        m_error = "VIDIOC_DQBUF returned invalid buffer index";
        return false;
    }

    const uint8_t* begin = static_cast<const uint8_t*>(m_buffers[buf.index].start);
    m_pending.assign(begin, begin + buf.bytesused);
    m_pending_pos = 0;

    if (xioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
        m_error = "VIDIOC_QBUF failed on " + m_config.device +
                  ": " + strerror(errno);
        return false;
    }
    return !m_pending.empty();
}

bool V4L2H264Source::next_nal(H264Nal& nal) {
    while (true) {
        if (parse_pending_nal(nal)) {
            return true;
        }
        if (!dequeue_encoded_buffer()) {
            return false;
        }
    }
}

std::unique_ptr<IH264Source> create_h264_source(const std::string& uri,
                                                uint32_t fps) {
    if (starts_with(uri, "v4l2:")) {
        V4L2H264SourceConfig config;
        config.device = uri.substr(strlen("v4l2:"));
        config.fps = fps;
        return std::unique_ptr<IH264Source>(new V4L2H264Source(config));
    }

    if (starts_with(uri, "/dev/video")) {
        V4L2H264SourceConfig config;
        config.device = uri;
        config.fps = fps;
        return std::unique_ptr<IH264Source>(new V4L2H264Source(config));
    }

    return std::unique_ptr<IH264Source>(new H264FileSource(uri));
}
