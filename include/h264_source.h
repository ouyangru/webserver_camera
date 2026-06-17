#ifndef H264_SOURCE_H
#define H264_SOURCE_H

#include <stdint.h>
#include <stddef.h>

#include <memory>
#include <string>
#include <vector>

#include "h264_parser.h"
#include "media_types.h"

class IH264Source {
public:
    virtual ~IH264Source() {}

    virtual bool good() const = 0;
    virtual const std::string& error() const = 0;
    virtual bool next_nal(H264Nal& nal) = 0;
};

class H264FileSource : public IH264Source {
public:
    explicit H264FileSource(const std::string& filename);

    bool good() const;
    const std::string& error() const;
    bool next_nal(H264Nal& nal);

private:
    H264Parser m_parser;
};

struct V4L2H264SourceConfig {
    std::string device;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t buffer_count;

    V4L2H264SourceConfig()
        : device("/dev/video0"),
          width(640),
          height(480),
          fps(25),
          buffer_count(4) {}
};

class V4L2H264Source : public IH264Source {
public:
    explicit V4L2H264Source(const V4L2H264SourceConfig& config);
    ~V4L2H264Source();

    bool good() const;
    const std::string& error() const;
    bool next_nal(H264Nal& nal);

private:
    struct Buffer {
        void* start;
        size_t length;

        Buffer() : start(NULL), length(0) {}
    };

    bool init_device();
    void close_device();
    bool dequeue_encoded_buffer();
    bool parse_pending_nal(H264Nal& nal);
    bool find_start_code(size_t from, size_t& position, size_t& length) const;

    V4L2H264SourceConfig m_config;
    int m_fd;
    bool m_streaming;
    std::vector<Buffer> m_buffers;
    std::vector<uint8_t> m_pending;
    size_t m_pending_pos;
    std::string m_error;
};

std::unique_ptr<IH264Source> create_h264_source(const std::string& uri,
                                                uint32_t fps);

#endif
