#include "stream_manager.h"

#include <algorithm>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <linux/videodev2.h>
#include <vector>

#include "flv_muxer.h"
#include "h264_source.h"
#include "http_conn.h"
#include "shared_buffer.h"

namespace {
const char* kMjpegBoundary = "frame";
const size_t kMaxQueueDepthDefault = 10;
const size_t kMaxFlvQueueDepthDefault = 100;

std::string json_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}
}

StreamManager::StreamManager()
    : m_dropped_packets(0),
      m_enqueued_packets(0),
      m_max_queue_depth(kMaxQueueDepthDefault),
      m_max_flv_queue_depth(kMaxFlvQueueDepthDefault),
      m_flv_thread_started(false),
      m_flv_stop_requested(false),
      m_flv_running(false),
      m_flv_ready(false),
      m_flv_input_path(""),
      m_flv_active_source(""),
      m_flv_last_error("H264 source is not configured"),
      m_flv_fps(25),
      m_flv_muxer(new FlvMuxer()) {
    pthread_mutex_init(&m_lock, NULL);
}

StreamManager::~StreamManager() {
    stop_flv_source();
    delete m_flv_muxer;
    pthread_mutex_destroy(&m_lock);
}

void StreamManager::lock() {
    pthread_mutex_lock(&m_lock);
}

void StreamManager::unlock() {
    pthread_mutex_unlock(&m_lock);
}

void StreamManager::add_mjpeg_client(http_conn* client) {
    if (!client) {
        return;
    }
    lock();
    for (size_t i = 0; i < m_mjpeg_clients.size(); ++i) {
        if (m_mjpeg_clients[i] == client) {
            unlock();
            return;
        }
    }
    m_mjpeg_clients.push_back(client);
    m_mjpeg_stats.clients = m_mjpeg_clients.size();
    unlock();

    pthread_mutex_lock(&g_frame.lock);
    g_frame.client_count++;
    if (g_frame.client_count == 1) {
        pthread_cond_signal(&g_frame.cond_start_cap);
    }
    pthread_mutex_unlock(&g_frame.lock);
}

void StreamManager::add_flv_client(http_conn* client) {
    if (!client) {
        return;
    }

    lock();
    for (size_t i = 0; i < m_flv_clients.size(); ++i) {
        if (m_flv_clients[i] == client) {
            unlock();
            return;
        }
    }
    m_flv_clients.push_back(client);
    m_flv_stats.clients = m_flv_clients.size();
    unlock();
}

void StreamManager::remove_client(http_conn* client) {
    if (!client) {
        return;
    }
    bool removed_mjpeg = false;
    lock();
    for (size_t i = 0; i < m_mjpeg_clients.size(); ++i) {
        if (m_mjpeg_clients[i] == client) {
            m_mjpeg_clients.erase(m_mjpeg_clients.begin() + i);
            removed_mjpeg = true;
            break;
        }
    }
    for (size_t i = 0; i < m_flv_clients.size(); ++i) {
        if (m_flv_clients[i] == client) {
            m_flv_clients.erase(m_flv_clients.begin() + i);
            break;
        }
    }
    m_mjpeg_stats.clients = m_mjpeg_clients.size();
    m_flv_stats.clients = m_flv_clients.size();
    unlock();

    if (!removed_mjpeg) {
        return;
    }

    pthread_mutex_lock(&g_frame.lock);
    if (g_frame.client_count > 0) {
        g_frame.client_count--;
    }
    pthread_mutex_unlock(&g_frame.lock);
}

bool StreamManager::is_mjpeg_source_ready() const {
    pthread_mutex_lock(&g_frame.lock);
    bool ready = g_frame.pixel_format == static_cast<int>(V4L2_PIX_FMT_MJPEG);
    pthread_mutex_unlock(&g_frame.lock);
    return ready;
}

bool StreamManager::is_flv_source_ready() {
    lock();
    bool ready = m_flv_ready;
    unlock();
    return ready;
}

bool StreamManager::is_flv_source_running() {
    lock();
    bool running = m_flv_running;
    unlock();
    return running;
}

bool StreamManager::start_flv_source(const std::string& input_path, uint32_t fps) {
    if (fps == 0 || fps > 240) {
        return false;
    }

    lock();
    if (m_flv_thread_started) {
        unlock();
        return true;
    }
    if (!input_path.empty()) {
        m_flv_input_path = input_path;
    }
    if (m_flv_input_path.empty()) {
        m_flv_last_error = "H264 source is not configured; pass an Annex-B .h264 file or a v4l2:/dev/videoX H264 encoder source";
        unlock();
        return false;
    }
    m_flv_fps = fps;
    m_flv_stop_requested = false;
    m_flv_running = false;
    m_flv_ready = false;
    m_flv_active_source.clear();
    m_flv_last_error.clear();
    m_flv_stats = StreamStats();
    m_flv_stats.running = false;
    m_flv_stats.ready = false;
    unlock();

    if (pthread_create(&m_flv_thread, NULL, flv_stream_thread, this) != 0) {
        lock();
        m_flv_thread_started = false;
        m_flv_running = false;
        m_flv_last_error = "failed to create FLV source thread";
        unlock();
        return false;
    }

    lock();
    m_flv_thread_started = true;
    unlock();
    return true;
}

void StreamManager::stop_flv_source() {
    lock();
    bool should_join = m_flv_thread_started;
    m_flv_stop_requested = true;
    unlock();

    if (should_join) {
        pthread_join(m_flv_thread, NULL);
    }

    lock();
    m_flv_thread_started = false;
    m_flv_running = false;
    m_flv_ready = false;
    m_flv_active_source.clear();
    m_flv_last_error = "H264 source stopped";
    m_flv_stats.running = false;
    m_flv_stats.ready = false;
    unlock();
}

void StreamManager::broadcast_mjpeg_frame(const unsigned char* data, size_t len) {
    if (!data || len == 0) {
        return;
    }

    char header_buf[128];
    int header_len = snprintf(header_buf, sizeof(header_buf),
                              "--%s\r\n"
                              "Content-Type: image/jpeg\r\n"
                              "Content-Length: %zu\r\n\r\n",
                              kMjpegBoundary, len);
    if (header_len <= 0) {
        return;
    }

    size_t packet_len = static_cast<size_t>(header_len) + len + 2;
    std::shared_ptr<std::vector<unsigned char>> packet =
        std::make_shared<std::vector<unsigned char>>(packet_len);

    memcpy(packet->data(), header_buf, static_cast<size_t>(header_len));
    memcpy(packet->data() + header_len, data, len);
    memcpy(packet->data() + header_len + len, "\r\n", 2);

    broadcast_mjpeg_packet(packet);
}

void StreamManager::broadcast_mjpeg_packet(const std::shared_ptr<std::vector<unsigned char>>& packet) {
    if (!packet || packet->empty()) {
        return;
    }

    lock();
    if (m_mjpeg_clients.empty()) {
        unlock();
        return;
    }
    std::vector<http_conn*> clients = m_mjpeg_clients;
    unlock();

    size_t enqueued = 0;
    size_t dropped = 0;
    for (size_t i = 0; i < clients.size(); ++i) {
        http_conn* client = clients[i];
        if (!client) {
            continue;
        }
        http_conn::PacketQueueResult result =
            client->enqueue_packet(packet, m_max_queue_depth, true);
        if (result != http_conn::PACKET_DROPPED) {
            ++enqueued;
        }
        if (result != http_conn::PACKET_ENQUEUED) {
            ++dropped;
        }
    }

    // 前5帧或每100帧打详细诊断
    {
        static int detail_count = 0;
        ++detail_count;
        if (detail_count <= 5 || detail_count % 100 == 0) {
            printf("[MJPEG] broadcast result: clients=%zu enqueued=%zu dropped=%zu\n",
                   clients.size(), enqueued, dropped);
        }
    }

    if (enqueued == 0 && m_mjpeg_stats.frames > 3) {
        static int warn_count = 0;
        if (++warn_count % 100 == 0) {
            printf("[MJPEG] WARNING: broadcast frame #%llu but enqueued=0, dropped=%zu, clients=%d\n",
                   (unsigned long long)m_mjpeg_stats.frames, dropped, (int)clients.size());
        }
    }


    lock();
    m_enqueued_packets += enqueued;
    m_dropped_packets += dropped;
    m_mjpeg_stats.enqueued_packets += enqueued;
    m_mjpeg_stats.dropped_packets += dropped;
    m_mjpeg_stats.frames++;
    m_mjpeg_stats.bytes += packet->size();
    m_mjpeg_stats.last_packet_size = packet->size();
    m_mjpeg_stats.clients = m_mjpeg_clients.size();
    m_mjpeg_stats.running = true;
    m_mjpeg_stats.ready = true;
    unlock();
}

void StreamManager::broadcast_flv_packet(const std::shared_ptr<Packet>& packet) {
    if (!packet || !packet->data || packet->data->empty()) {
        return;
    }

    lock();
    std::vector<http_conn*> clients = m_flv_clients;
    unlock();

    size_t enqueued = 0;
    size_t dropped = 0;
    for (size_t i = 0; i < clients.size(); ++i) {
        http_conn* client = clients[i];
        if (!client) {
            continue;
        }
        http_conn::PacketQueueResult result = client->enqueue_flv_packet(packet);
        if (result != http_conn::PACKET_DROPPED) {
            ++enqueued;
        }
        if (result != http_conn::PACKET_ENQUEUED) {
            ++dropped;
        }
    }

    lock();
    m_enqueued_packets += enqueued;
    m_dropped_packets += dropped;
    m_flv_stats.enqueued_packets += enqueued;
    m_flv_stats.dropped_packets += dropped;
    if (!packet->is_config) {
        m_flv_stats.frames++;
        if (packet->is_key_frame) {
            m_flv_stats.key_frames++;
        }
    }
    m_flv_stats.bytes += packet->size();
    m_flv_stats.last_packet_size = packet->size();
    m_flv_stats.clients = m_flv_clients.size();
    unlock();
}

std::shared_ptr<Packet> StreamManager::make_flv_header_packet() {
    lock();
    std::shared_ptr<Packet> packet = m_flv_muxer->make_flv_header();
    unlock();
    return packet;
}

std::shared_ptr<Packet> StreamManager::make_flv_sequence_header_packet() {
    lock();
    std::shared_ptr<Packet> packet;
    if (m_flv_muxer->ready()) {
        packet = m_flv_muxer->make_avc_sequence_header(0);
    }
    unlock();
    return packet;
}

size_t StreamManager::mjpeg_client_count() {
    lock();
    size_t count = m_mjpeg_clients.size();
    unlock();
    return count;
}

size_t StreamManager::flv_client_count() {
    lock();
    size_t count = m_flv_clients.size();
    unlock();
    return count;
}

size_t StreamManager::dropped_packets() {
    lock();
    size_t count = m_dropped_packets;
    unlock();
    return count;
}

size_t StreamManager::enqueued_packets() {
    lock();
    size_t count = m_enqueued_packets;
    unlock();
    return count;
}

std::string StreamManager::status_json() {
    pthread_mutex_lock(&g_frame.lock);
    size_t frame_len = g_frame.length;
    int pixel_format = g_frame.pixel_format;
    pthread_mutex_unlock(&g_frame.lock);

    lock();
    m_mjpeg_stats.clients = m_mjpeg_clients.size();
    m_flv_stats.clients = m_flv_clients.size();
    m_flv_stats.running = m_flv_running;
    m_flv_stats.ready = m_flv_ready;

    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{"
             "\"mjpeg\":{\"clients\":%zu,\"frame_bytes\":%zu,"
             "\"pixel_format\":%d,\"frames\":%llu,\"bytes\":%llu,"
             "\"enqueued\":%llu,\"dropped\":%llu},"
             "\"flv\":{\"clients\":%zu,\"running\":%s,\"ready\":%s,"
             "\"source\":\"%s\",\"active_source\":\"%s\","
             "\"last_error\":\"%s\","
             "\"fps\":%.2f,\"frames\":%llu,"
             "\"key_frames\":%llu,\"bytes\":%llu,\"last_packet_size\":%zu,"
             "\"enqueued\":%llu,\"dropped\":%llu}"
             "}\n",
             m_mjpeg_clients.size(), frame_len, pixel_format,
             static_cast<unsigned long long>(m_mjpeg_stats.frames),
             static_cast<unsigned long long>(m_mjpeg_stats.bytes),
             static_cast<unsigned long long>(m_mjpeg_stats.enqueued_packets),
             static_cast<unsigned long long>(m_mjpeg_stats.dropped_packets),
             m_flv_clients.size(),
             m_flv_running ? "true" : "false",
             m_flv_ready ? "true" : "false",
             json_escape(m_flv_input_path).c_str(),
             json_escape(m_flv_active_source).c_str(),
             json_escape(m_flv_last_error).c_str(),
             m_flv_stats.fps,
             static_cast<unsigned long long>(m_flv_stats.frames),
             static_cast<unsigned long long>(m_flv_stats.key_frames),
             static_cast<unsigned long long>(m_flv_stats.bytes),
             m_flv_stats.last_packet_size,
             static_cast<unsigned long long>(m_flv_stats.enqueued_packets),
             static_cast<unsigned long long>(m_flv_stats.dropped_packets));
    std::string body(buf);
    unlock();
    return body;
}

void StreamManager::run_flv_source() {
    uint64_t frame_index = 0;

    while (true) {
        lock();
        bool stop_requested = m_flv_stop_requested;
        std::string input_path = m_flv_input_path;
        uint32_t fps = m_flv_fps;
        m_flv_running = !stop_requested;
        m_flv_stats.running = !stop_requested;
        unlock();

        if (stop_requested) {
            break;
        }

        std::unique_ptr<IH264Source> source = create_h264_source(input_path, fps);
        std::string active_source = input_path;
        if (!source || !source->good()) {
            const std::string error_text = source ? source->error() : "failed to create H264 source";
            fprintf(stderr, "[HTTP-FLV] %s\n", error_text.c_str());
            lock();
            m_flv_running = false;
            m_flv_ready = false;
            m_flv_active_source.clear();
            m_flv_last_error = error_text;
            m_flv_stats.running = false;
            m_flv_stats.ready = false;
            unlock();
            sleep(1);
            continue;
        }

        lock();
        m_flv_active_source = active_source;
        m_flv_last_error.clear();
        unlock();

        FlvMuxer local_muxer;
        H264AccessUnitAssembler assembler;
        H264Nal nal;
        bool config_dirty = false;

        const uint32_t sleep_us = 1000000u / std::max<uint32_t>(fps, 1);

        while (source->next_nal(nal)) {
            lock();
            stop_requested = m_flv_stop_requested;
            unlock();
            if (stop_requested) {
                break;
            }

            if (nal.is_sps() || nal.is_pps()) {
                H264AccessUnit completed;
                if (assembler.flush(completed) && !completed.empty() && local_muxer.ready()) {
                    uint64_t timestamp = frame_index * 1000 / fps;
                    completed.dts_ms = timestamp;
                    completed.pts_ms = timestamp;
                    std::shared_ptr<Packet> pkt = local_muxer.make_video_tag(completed);
                    broadcast_flv_packet(pkt);
                    ++frame_index;
                    usleep(sleep_us);
                }

                if (nal.is_sps()) {
                    local_muxer.set_sps(nal.data);
                } else {
                    local_muxer.set_pps(nal.data);
                }

                config_dirty = true;

                if (local_muxer.ready()) {
                    lock();
                    *m_flv_muxer = local_muxer;
                    m_flv_ready = true;
                    m_flv_last_error.clear();
                    m_flv_stats.ready = true;
                    m_flv_stats.fps = static_cast<double>(fps);
                    unlock();
                }
                continue;
            }

            H264AccessUnit completed;
            if (!assembler.push(nal, completed) || completed.empty()) {
                continue;
            }
            if (!local_muxer.ready()) {
                continue;
            }

            uint64_t timestamp = frame_index * 1000 / fps;
            completed.dts_ms = timestamp;
            completed.pts_ms = timestamp;

            if (config_dirty) {
                lock();
                *m_flv_muxer = local_muxer;
                m_flv_ready = true;
                m_flv_last_error.clear();
                m_flv_stats.ready = true;
                m_flv_stats.fps = static_cast<double>(fps);
                unlock();
                broadcast_flv_packet(local_muxer.make_avc_sequence_header(
                    static_cast<uint32_t>(timestamp)));
                config_dirty = false;
            }

            broadcast_flv_packet(local_muxer.make_video_tag(completed));
            ++frame_index;
            usleep(sleep_us);
        }

        H264AccessUnit completed;
        if (!stop_requested && assembler.flush(completed) && !completed.empty() && local_muxer.ready()) {
            uint64_t timestamp = frame_index * 1000 / fps;
            completed.dts_ms = timestamp;
            completed.pts_ms = timestamp;
            broadcast_flv_packet(local_muxer.make_video_tag(completed));
            ++frame_index;
            usleep(sleep_us);
        }
    }

    lock();
    m_flv_running = false;
    m_flv_stats.running = false;
    unlock();
}

void* mjpeg_stream_thread(void* arg) {
    StreamManager* manager = reinterpret_cast<StreamManager*>(arg);
    if (!manager) {
        return NULL;
    }

    printf("[MJPEG] broadcast thread started\n");
    uint64_t last_sequence = 0;
    int frame_count = 0;
    while (true) {
        pthread_mutex_lock(&g_frame.lock);
        while (g_frame.sequence == last_sequence) {
            pthread_cond_wait(&g_frame.cond_new_frame, &g_frame.lock);
        }
        last_sequence = g_frame.sequence;

        if (g_frame.length == 0 ||
            g_frame.pixel_format != static_cast<int>(V4L2_PIX_FMT_MJPEG)) {
            pthread_mutex_unlock(&g_frame.lock);
            continue;
        }

        // 搜索真正的 JPEG 边界（SOI=0xFFD8 和 EOI=0xFFD9）
        // 因为某些 V4L2 摄像头在 JPEG 数据前可能插入厂商自定义头
        size_t raw_len = g_frame.length;
        unsigned char* raw_data = g_frame.data;

        // 从前往后找 SOI
        size_t jpeg_start = 0;
        bool found_soi = false;
        for (size_t i = 0; i + 1 < raw_len; i++) {
            if (raw_data[i] == 0xFF && raw_data[i+1] == 0xD8) {
                jpeg_start = i;
                found_soi = true;
                break;
            }
        }

        // 从后往前找 EOI
        size_t jpeg_end = raw_len;
        bool found_eoi = false;
        for (size_t i = raw_len; i >= 2; i--) {
            if (raw_data[i-2] == 0xFF && raw_data[i-1] == 0xD9) {
                jpeg_end = i;
                found_eoi = true;
                break;
            }
        }

        if (!found_soi || !found_eoi) {
            static int no_marker_count = 0;
            if (++no_marker_count % 100 == 0) {
                printf("[MJPEG] SKIP frame: no SOI/EOI in %zu bytes "
                       "(SOI=%s EOI=%s)\n",
                       raw_len,
                       found_soi ? "found" : "missing",
                       found_eoi ? "found" : "missing");
            }
            pthread_mutex_unlock(&g_frame.lock);
            continue;
        }

        size_t jpeg_len = jpeg_end - jpeg_start;
        unsigned char* jpeg_data_start = raw_data + jpeg_start;
        
        // 仍用锁保护，打印诊断
        int jpeg_soi_ok = (jpeg_data_start[0] == 0xFF && jpeg_data_start[1] == 0xD8);
        int jpeg_eoi_ok = (jpeg_data_start[jpeg_len-2] == 0xFF && jpeg_data_start[jpeg_len-1] == 0xD9);

        char header_buf[128];
        int header_len = snprintf(header_buf, sizeof(header_buf),
                                  "--%s\r\n"
                                  "Content-Type: image/jpeg\r\n"
                                  "Content-Length: %zu\r\n\r\n",
                                  kMjpegBoundary, jpeg_len);
        if (header_len <= 0) {
            pthread_mutex_unlock(&g_frame.lock);
            continue;
        }

        size_t packet_len = static_cast<size_t>(header_len) + jpeg_len + 2;
        std::shared_ptr<std::vector<unsigned char>> packet =
            std::make_shared<std::vector<unsigned char>>(packet_len);

        memcpy(packet->data(), header_buf, static_cast<size_t>(header_len));
        memcpy(packet->data() + header_len, jpeg_data_start, jpeg_len);
        memcpy(packet->data() + header_len + jpeg_len, "\r\n", 2);
        
        pthread_mutex_unlock(&g_frame.lock);

        frame_count++;
        if (frame_count <= 5 || frame_count % 50 == 0) {
            printf("[MJPEG] frame #%d seq=%lu raw=%zu jpeg=%zu (offset=%zu) SOI=%s EOI=%s first16=",
                   frame_count, (unsigned long)last_sequence,
                   raw_len, jpeg_len, jpeg_start,
                   jpeg_soi_ok ? "OK" : "MISSING",
                   jpeg_eoi_ok ? "OK" : "MISSING");
            for (size_t b = 0; b < 16 && b < jpeg_len; b++)
                printf("%02X ", jpeg_data_start[b]);
            printf(" last4=");
            for (size_t b = jpeg_len > 4 ? jpeg_len - 4 : 0; b < jpeg_len; b++)
                printf("%02X ", jpeg_data_start[b]);
            printf("\n");
        }
        if (!jpeg_soi_ok || !jpeg_eoi_ok) {
            printf("[MJPEG] WARN invalid JPEG frame #%d SOI=%s EOI=%s first16=",
                   frame_count,
                   jpeg_soi_ok ? "OK" : "MISSING",
                   jpeg_eoi_ok ? "OK" : "MISSING");
            for (size_t b = 0; b < 16 && b < jpeg_len; b++)
                printf("%02X ", jpeg_data_start[b]);
            printf("\n");
        }
        manager->broadcast_mjpeg_packet(packet);
    }

    return NULL;
}

void* flv_stream_thread(void* arg) {
    StreamManager* manager = reinterpret_cast<StreamManager*>(arg);
    if (!manager) {
        return NULL;
    }
    manager->run_flv_source();
    return NULL;
}
