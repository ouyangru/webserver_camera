#ifndef STREAM_MANAGER_H
#define STREAM_MANAGER_H

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <memory>
#include <string>

class http_conn;
class FlvMuxer;
struct Packet;

struct StreamStats {
    size_t clients;
    uint64_t frames;
    uint64_t bytes;
    uint64_t enqueued_packets;
    uint64_t dropped_packets;
    uint64_t key_frames;
    size_t last_packet_size;
    double fps;
    bool running;
    bool ready;

    StreamStats()
        : clients(0),
          frames(0),
          bytes(0),
          enqueued_packets(0),
          dropped_packets(0),
          key_frames(0),
          last_packet_size(0),
          fps(0.0),
          running(false),
          ready(false) {}
};

class StreamManager {
public:
    StreamManager();
    ~StreamManager();

    void add_mjpeg_client(http_conn* client);
    void add_flv_client(http_conn* client);
    void remove_client(http_conn* client);
    bool is_mjpeg_source_ready() const;
    bool is_flv_source_ready();
    bool is_flv_source_running();

    bool start_flv_source(const std::string& input_path, uint32_t fps);
    void stop_flv_source();

    void broadcast_mjpeg_frame(const unsigned char* data, size_t len);
    void broadcast_mjpeg_packet(const std::shared_ptr<std::vector<unsigned char>>& packet);
    void broadcast_flv_packet(const std::shared_ptr<Packet>& packet);

    std::shared_ptr<Packet> make_flv_header_packet();
    std::shared_ptr<Packet> make_flv_sequence_header_packet();

    size_t mjpeg_client_count();
    size_t flv_client_count();
    size_t dropped_packets();
    size_t enqueued_packets();
    std::string status_json();

private:
    friend void* flv_stream_thread(void* arg);

    void lock();
    void unlock();
    void run_flv_source();

    mutable pthread_mutex_t m_lock;
    std::vector<http_conn*> m_mjpeg_clients;
    std::vector<http_conn*> m_flv_clients;
    size_t m_dropped_packets;
    size_t m_enqueued_packets;
    size_t m_max_queue_depth;
    size_t m_max_flv_queue_depth;

    pthread_t m_flv_thread;
    bool m_flv_thread_started;
    bool m_flv_stop_requested;
    bool m_flv_running;
    bool m_flv_ready;
    std::string m_flv_input_path;
    std::string m_flv_active_source;
    std::string m_flv_last_error;
    uint32_t m_flv_fps;

    FlvMuxer* m_flv_muxer;
    StreamStats m_mjpeg_stats;
    StreamStats m_flv_stats;
};

void* mjpeg_stream_thread(void* arg);
void* flv_stream_thread(void* arg);

#endif
