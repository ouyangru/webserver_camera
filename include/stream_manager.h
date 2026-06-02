#ifndef STREAM_MANAGER_H
#define STREAM_MANAGER_H

#include <pthread.h>
#include <stddef.h>
#include <vector>
#include <memory>

class http_conn;

class StreamManager {
public:
    StreamManager();
    ~StreamManager();

    void add_mjpeg_client(http_conn* client);
    void remove_client(http_conn* client);

    void broadcast_mjpeg_frame(const unsigned char* data, size_t len);
    void broadcast_mjpeg_packet(const std::shared_ptr<std::vector<unsigned char>>& packet);

    size_t mjpeg_client_count();
    size_t dropped_packets();
    size_t enqueued_packets();

private:
    void lock();
    void unlock();

    mutable pthread_mutex_t m_lock;
    std::vector<http_conn*> m_mjpeg_clients;
    size_t m_dropped_packets;
    size_t m_enqueued_packets;
    size_t m_max_queue_depth;
};

void* mjpeg_stream_thread(void* arg);

#endif
