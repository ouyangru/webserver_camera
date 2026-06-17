#include "h264_parser.h"

#include <fstream>
#include <iterator>
#include <utility>

namespace {

class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t>& bytes)
        : m_bytes(bytes), m_bit_position(0) {}

    bool read_bit(uint32_t& bit) {
        if (m_bit_position >= m_bytes.size() * 8) {
            return false;
        }
        size_t byte_index = m_bit_position / 8;
        size_t bit_index = 7 - (m_bit_position % 8);
        bit = (m_bytes[byte_index] >> bit_index) & 0x01;
        ++m_bit_position;
        return true;
    }

    bool read_ue(uint32_t& value) {
        size_t leading_zero_bits = 0;
        uint32_t bit = 0;
        while (read_bit(bit) && bit == 0) {
            ++leading_zero_bits;
            if (leading_zero_bits > 31) {
                return false;
            }
        }
        if (bit != 1) {
            return false;
        }

        uint32_t suffix = 0;
        for (size_t i = 0; i < leading_zero_bits; ++i) {
            if (!read_bit(bit)) {
                return false;
            }
            suffix = (suffix << 1) | bit;
        }

        value = ((1u << leading_zero_bits) - 1u) + suffix;
        return true;
    }

private:
    const std::vector<uint8_t>& m_bytes;
    size_t m_bit_position;
};

std::vector<uint8_t> make_rbsp(const H264Nal& nal) {
    std::vector<uint8_t> rbsp;
    if (nal.data.size() <= 1) {
        return rbsp;
    }

    rbsp.reserve(nal.data.size() - 1);
    int consecutive_zeros = 0;
    for (size_t i = 1; i < nal.data.size(); ++i) {
        uint8_t byte = nal.data[i];
        if (consecutive_zeros >= 2 && byte == 0x03) {
            consecutive_zeros = 0;
            continue;
        }
        rbsp.push_back(byte);
        consecutive_zeros = (byte == 0x00) ? consecutive_zeros + 1 : 0;
    }
    return rbsp;
}

} // namespace

H264Parser::H264Parser(const std::string& filename)
    : m_position(0) {
    std::ifstream input(filename.c_str(), std::ios::binary);
    if (!input) {
        m_error = "failed to open H264 input: " + filename;
        return;
    }

    m_buffer.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
    if (m_buffer.empty()) {
        m_error = "H264 input is empty: " + filename;
    }
}

bool H264Parser::good() const {
    return m_error.empty();
}

const std::string& H264Parser::error() const {
    return m_error;
}

bool H264Parser::find_start_code(size_t from,
                                 size_t& position,
                                 size_t& length) const {
    for (size_t i = from; i + 3 <= m_buffer.size(); ++i) {
        if (m_buffer[i] != 0x00 || m_buffer[i + 1] != 0x00) {
            continue;
        }
        if (m_buffer[i + 2] == 0x01) {
            position = i;
            length = 3;
            return true;
        }
        if (i + 4 <= m_buffer.size() &&
            m_buffer[i + 2] == 0x00 &&
            m_buffer[i + 3] == 0x01) {
            position = i;
            length = 4;
            return true;
        }
    }
    return false;
}

bool H264Parser::next_nal(H264Nal& nal) {
    nal.data.clear();

    size_t start = 0;
    size_t start_code_length = 0;
    if (!find_start_code(m_position, start, start_code_length)) {
        m_position = m_buffer.size();
        return false;
    }

    size_t nal_start = start + start_code_length;
    size_t next_start = 0;
    size_t next_start_code_length = 0;
    if (!find_start_code(nal_start, next_start, next_start_code_length)) {
        next_start = m_buffer.size();
    }

    while (next_start > nal_start && m_buffer[next_start - 1] == 0x00) {
        --next_start;
    }

    m_position = (next_start < m_buffer.size()) ? next_start : m_buffer.size();
    if (next_start <= nal_start) {
        return next_nal(nal);
    }

    nal.data.assign(m_buffer.begin() + nal_start, m_buffer.begin() + next_start);
    return !nal.data.empty();
}

H264AccessUnitAssembler::H264AccessUnitAssembler()
    : m_has_vcl(false) {}

void H264AccessUnitAssembler::begin_access_unit_if_needed() {
    if (m_has_vcl) {
        return;
    }
    m_current.nals.insert(m_current.nals.end(),
                          m_pending_non_vcl.begin(),
                          m_pending_non_vcl.end());
    m_pending_non_vcl.clear();
}

bool H264AccessUnitAssembler::first_mb_in_slice_is_zero(
        const H264Nal& nal) const {
    std::vector<uint8_t> rbsp = make_rbsp(nal);
    BitReader reader(rbsp);
    uint32_t first_mb_in_slice = 0;
    return reader.read_ue(first_mb_in_slice) && first_mb_in_slice == 0;
}

bool H264AccessUnitAssembler::push(const H264Nal& nal,
                                   H264AccessUnit& completed) {
    completed = H264AccessUnit();

    if (nal.is_sps() || nal.is_pps()) {
        return false;
    }

    if (nal.type() == 9) {
        return flush(completed);
    }

    if (!nal.is_vcl()) {
        m_pending_non_vcl.push_back(nal);
        return false;
    }

    if (m_has_vcl && first_mb_in_slice_is_zero(nal)) {
        completed = std::move(m_current);
        m_current = H264AccessUnit();
        m_has_vcl = false;
        begin_access_unit_if_needed();
        m_current.nals.push_back(nal);
        m_has_vcl = true;
        return true;
    }

    begin_access_unit_if_needed();
    m_current.nals.push_back(nal);
    m_has_vcl = true;
    return false;
}

bool H264AccessUnitAssembler::flush(H264AccessUnit& completed) {
    completed = H264AccessUnit();
    if (!m_has_vcl || m_current.empty()) {
        return false;
    }

    completed = std::move(m_current);
    m_current = H264AccessUnit();
    m_has_vcl = false;
    return true;
}
