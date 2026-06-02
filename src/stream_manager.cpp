#include "stream_manager.h"

#include <stdio.h>
#include <string.h>
#include <vector>

#include "http_conn.h"
#include "shared_buffer.h"

namespace {
const char* kMjpegBoundary = "frame";
const size_t kMaxQueueDepthDefault = 5;
}

StreamManager::StreamManager()
    : m_dropped_packets(0),
      m_enqueued_packets(0),
      m_max_queue_depth(kMaxQueueDepthDefault) {
    pthread_mutex_init(&m_lock, NULL);
}

StreamManager::~StreamManager() {
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
    unlock();

    pthread_mutex_lock(&g_frame.lock);
    g_frame.client_count++;
    if (g_frame.client_count == 1) {
        pthread_cond_signal(&g_frame.cond_start_cap);
    }
    pthread_mutex_unlock(&g_frame.lock);
}

void StreamManager::remove_client(http_conn* client) {
    if (!client) {
        return;
    }
    lock();
    for (size_t i = 0; i < m_mjpeg_clients.size(); ++i) {
        if (m_mjpeg_clients[i] == client) {
            m_mjpeg_clients.erase(m_mjpeg_clients.begin() + i);
            break;
        }
    }
    unlock();

    pthread_mutex_lock(&g_frame.lock);
    if (g_frame.client_count > 0) {
        g_frame.client_count--;
    }
    pthread_mutex_unlock(&g_frame.lock);
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

    for (size_t i = 0; i < m_mjpeg_clients.size(); ++i) {
        http_conn* client = m_mjpeg_clients[i];
        if (!client) {
            continue;
        }
        bool dropped = client->enqueue_packet(packet, m_max_queue_depth);
        m_enqueued_packets++;
        if (dropped) {
            m_dropped_packets++;
        }
    }
    unlock();
}

size_t StreamManager::mjpeg_client_count() {
    lock();
    size_t count = m_mjpeg_clients.size();
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

void* mjpeg_stream_thread(void* arg) {
    StreamManager* manager = reinterpret_cast<StreamManager*>(arg);
    if (!manager) {
        return NULL;
    }

    while (true) {
        pthread_mutex_lock(&g_frame.lock);
        pthread_cond_wait(&g_frame.cond_new_frame, &g_frame.lock);
        if (g_frame.length == 0) {
            pthread_mutex_unlock(&g_frame.lock);
            continue;
        }
        size_t len = g_frame.length;

        char header_buf[128];
        int header_len = snprintf(header_buf, sizeof(header_buf),
                                  "--%s\r\n"
                                  "Content-Type: image/jpeg\r\n"
                                  "Content-Length: %zu\r\n\r\n",
                                  kMjpegBoundary, len);
        if (header_len <= 0) {
            pthread_mutex_unlock(&g_frame.lock);
            continue;
        }

        size_t packet_len = static_cast<size_t>(header_len) + len + 2;
        std::shared_ptr<std::vector<unsigned char>> packet =
            std::make_shared<std::vector<unsigned char>>(packet_len);

        memcpy(packet->data(), header_buf, static_cast<size_t>(header_len));
        memcpy(packet->data() + header_len, g_frame.data, len);
        memcpy(packet->data() + header_len + len, "\r\n", 2);
        pthread_mutex_unlock(&g_frame.lock);

        manager->broadcast_mjpeg_packet(packet);
    }

    return NULL;
}
