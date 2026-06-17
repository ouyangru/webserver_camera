#ifndef H264_PARSER_H
#define H264_PARSER_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "media_types.h"

class H264Parser {
public:
    explicit H264Parser(const std::string& filename);

    bool good() const;
    const std::string& error() const;
    bool next_nal(H264Nal& nal);

private:
    bool find_start_code(size_t from, size_t& position, size_t& length) const;

    std::vector<uint8_t> m_buffer;
    size_t m_position;
    std::string m_error;
};

class H264AccessUnitAssembler {
public:
    H264AccessUnitAssembler();

    // 返回 true 表示 completed 中产生了一帧完整的 access unit。
    bool push(const H264Nal& nal, H264AccessUnit& completed);
    bool flush(H264AccessUnit& completed);

private:
    bool first_mb_in_slice_is_zero(const H264Nal& nal) const;
    void begin_access_unit_if_needed();

    H264AccessUnit m_current;
    std::vector<H264Nal> m_pending_non_vcl;
    bool m_has_vcl;
};

#endif
