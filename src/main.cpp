#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <libgen.h>
#include <new>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <limits.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "v4l2_capture.h"
#include "shared_buffer.h"
#if ENABLE_GSTREAMER
#include "rtsp_server.h"
#endif
#include "stream_manager.h"
#include "logger.h"
#include "pipeline_metrics.h"
#include "cv_frame_bus.h"


#define MAX_FD 1024
#define MAX_EVENT_NUMBER 64

// 添加文件描述符
extern void addfd( int epollfd, int fd, bool one_shot );
extern void removefd( int epollfd, int fd );

// 添加信号捕捉
void addsig(int sig, void( handler )(int)){
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

bool uses_default_v4l2_device(const char* source) {
    if (!source) {
        return false;
    }
    return strcmp(source, "/dev/video0") == 0 ||
           strcmp(source, "v4l2:/dev/video0") == 0;
}

// 通过 /proc/self/exe 获取可执行文件真实路径，自动推导 resources 目录
// 规则：可执行文件在 bin/ 下，resources 在 ../resources（相对于可执行文件目录）
static std::string get_resources_path() {
    const char* env_path = getenv("WEBSERVER_CAMERA_RESOURCES");
    if (env_path && env_path[0] != '\0') {
        char resolved_env[PATH_MAX];
        if (realpath(env_path, resolved_env) != NULL) {
            return std::string(resolved_env);
        }
        return std::string(env_path);
    }

    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, PATH_MAX - 1);
    if (len == -1) {
        // fallback：使用当前工作目录下的 resources
        return "./resources";
    }
    exe_path[len] = '\0';

    // 提取可执行文件所在目录（如 /home/user/project/bin）
    char exe_path_copy[PATH_MAX];
    strncpy(exe_path_copy, exe_path, PATH_MAX - 1);
    exe_path_copy[PATH_MAX - 1] = '\0';
    char* dir = dirname(exe_path_copy);

    // 拼接 ../resources（bin/../resources = project/resources）
    std::string resources_dir = std::string(dir) + "/../resources";

    // 规范化路径（解析 ../ 等）
    char resolved[PATH_MAX];
    if (realpath(resources_dir.c_str(), resolved) == NULL) {
        const char* installed_resources_dir = "/usr/share/webserver_camera";
        if (realpath(installed_resources_dir, resolved) != NULL) {
            return std::string(resolved);
        }
        return resources_dir; // 后续检查会失败并报出原始推导路径
    }
    return std::string(resolved);
}

int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {
        printf( "usage: %s port_number [h264_file|v4l2:/dev/videoX]\n", basename(argv[0]));
        printf( "\n");
        printf( "  port_number        HTTP server listen port\n");
        printf( "  h264_file          Local .h264 Annex-B file for HTTP-FLV stream\n");
        printf( "  v4l2:/dev/videoX   V4L2 H264 encoder device for HTTP-FLV stream\n");
        printf( "\n");
        printf( "MJPEG stream (/mjpeg) always uses /dev/video0 for capture.\n");
        printf( "FLV stream (/live.flv) uses the optional second argument.\n");
        printf( "On V853: MJPEG capture and H264 encoding are separate /dev/videoX nodes.\n");
        printf( "\n");
        printf( "Examples:\n");
        printf( "  %s 2000                    # MJPEG only (no FLV source)\n", basename(argv[0]));
        printf( "  %s 2000 test.h264          # MJPEG + FLV from file\n", basename(argv[0]));
        printf( "  %s 2000 v4l2:/dev/video3   # MJPEG + FLV from H264 encoder\n", basename(argv[0]));
        return 1;
    }

    std::string resources_dir = get_resources_path();
    struct stat st;
    if (stat(resources_dir.c_str(), &st) == -1 || !S_ISDIR(st.st_mode)) {
        LOG_ERROR("main", "event=resources_unavailable path=%s", resources_dir.c_str());
        return 1;
    }
    http_conn::set_doc_root(resources_dir);
    LOG_INFO("main", "event=resources path=%s", resources_dir.c_str());

    // 获取端口号
    int port = atoi( argv[1] );
    if (port <= 0 || port > 65535) {
        LOG_ERROR("main", "event=invalid_port value=%s", argv[1]);
        return 1;
    }

    LOG_INFO("main", "event=server_start port=%d", port);
    perf_start();
    cv_set_mode(0);

    // 对SIGPIE信号进行处理
    addsig( SIGPIPE, SIG_IGN );
    
    // 创建和初始化线程池
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>(2, 64);
    } catch( ... ) {
        LOG_ERROR("main", "event=threadpool_create_failed");
        return 1;
    }

    StreamManager stream_manager;
    http_conn::set_stream_manager(&stream_manager);
    const char* flv_input_path = (argc >= 3) ? argv[2] : NULL;
#if ENABLE_CEDAR
    if (!flv_input_path) flv_input_path = "cedar";
#endif

    // 创建一个数组 用于保存所有的客户端信息
    // Stable, lazily allocated slots: broadcaster/worker raw pointers must not
    // become dangling during close. Retain slots for safe reuse until shutdown.
    std::unique_ptr<http_conn>* users = new (std::nothrow) std::unique_ptr<http_conn>[MAX_FD];
    if (!users) {
        LOG_ERROR("main", "event=connection_table_allocation_failed");
        delete pool;
        return 1;
    }


    // 创建监听套接字
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    if (listenfd < 0) {
        LOG_ERROR("main", "event=socket_failed errno=%d", errno);
        delete [] users;
        delete pool;
        return 1;
    }

    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    // 端口复用
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    if (ret < 0) {
        LOG_ERROR("main", "event=bind_failed errno=%d", errno);
        close(listenfd);
        delete [] users;
        delete pool;
        return 1;
    }

    // 监听
    ret = listen( listenfd, 5 );
    if (ret < 0) {
        LOG_ERROR("main", "event=listen_failed errno=%d", errno);
        close(listenfd);
        delete [] users;
        delete pool;
        return 1;
    }

    LOG_INFO("main", "event=listening fd=%d port=%d backlog=5", listenfd, port);

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create1(0);
    if (epollfd < 0) {
        LOG_ERROR("main", "event=epoll_create1_failed errno=%d", errno);
        close(listenfd);
        delete [] users;
        delete pool;
        return 1;
    }
    // 添加到epoll对象中
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd;

    // MJPEG 采集：固定使用 /dev/video0，采集 MJPEG 帧用于 /mjpeg 流
    // 与 FLV 源独立：FLV 可以是 .h264 文件或另一个 v4l2 H264 编码器设备
    {
        pthread_t v4l2_tid;
        pthread_create(&v4l2_tid, NULL, v4l2_thread_func, NULL);
        pthread_detach(v4l2_tid);

        pthread_t mjpeg_tid;
        pthread_create(&mjpeg_tid, NULL, mjpeg_stream_thread, &stream_manager);
        pthread_detach(mjpeg_tid);
    }

    // FLV 源：可以是 .h264 文件 或 v4l2:/dev/videoX（H264 硬编码器设备）
    // 如果 FLV 源也是 /dev/video0，V4L2 open 会失败，FLV 降级为不可用
    // 在 V853 上，MJPEG 采集和 H264 编码通常是不同的 /dev/videoX 节点
    if (flv_input_path) {
        if (uses_default_v4l2_device(flv_input_path)) {
            printf("[HTTP-FLV] WARNING: FLV source is /dev/video0, same as MJPEG capture device.\n");
            printf("[HTTP-FLV]          On V853, use v4l2:/dev/videoX (H264 encoder node) instead.\n");
            printf("[HTTP-FLV]          Example: %s %d v4l2:/dev/video3\n", basename(argv[0]), port);
        }
        if (!stream_manager.start_flv_source(flv_input_path,
                strcmp(flv_input_path, "cedar") == 0 ? 15 : 25)) {
            fprintf(stderr, "[HTTP-FLV] failed to start source: %s\n", flv_input_path);
        }
    } else {
        printf("[HTTP-FLV] no H264 source configured; /live.flv stays unavailable.\n");
        printf("[HTTP-FLV] To enable: %s %d test.h264\n", basename(argv[0]), port);
        printf("[HTTP-FLV]        or: %s %d v4l2:/dev/videoX\n", basename(argv[0]), port);
    }

#if ENABLE_GSTREAMER
    set_hls_resources_dir(resources_dir.c_str());
    pthread_t rtsp_tid;
    pthread_create(&rtsp_tid, NULL, rtsp_server_thread, NULL);
    pthread_detach(rtsp_tid);

    pthread_t hls_tid;
    pthread_create(&hls_tid, NULL, hls_streamer_thread, NULL);
    pthread_detach(hls_tid);
#else
    printf("[Media] GStreamer disabled; RTSP and HLS will not start.\n");
#endif

    while(true) {
        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            LOG_ERROR("epoll", "event=wait_failed epollfd=%d errno=%d", epollfd, errno);
            break;
        }

        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            
            if( sockfd == listenfd ) {
                // 有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                
                if ( connfd < 0 ) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                        LOG_TRACE("main", "event=accept_retry listen_fd=%d errno=%d", listenfd, errno);
                    } else {
                        LOG_ERROR("main", "event=accept_failed listen_fd=%d errno=%d", listenfd, errno);
                    }
                    continue;
                } 

                char peer[INET_ADDRSTRLEN] = {};
                int nodelay=1;
                setsockopt(connfd,IPPROTO_TCP,TCP_NODELAY,&nodelay,sizeof(nodelay));
                const char* peer_text = inet_ntop(AF_INET, &client_address.sin_addr, peer, sizeof(peer));
                LOG_INFO("main", "event=accept listen_fd=%d fd=%d peer=%s port=%u",
                         listenfd, connfd, peer_text ? peer_text : "unknown",
                         static_cast<unsigned int>(ntohs(client_address.sin_port)));

                if (connfd >= MAX_FD || http_conn::m_user_count.load() >= 32) {
                    // 目前连接数满了
                    // 给客户端写一个信息： 服务器正忙
                    LOG_WARN("main", "event=reject fd=%d reason=connection_limit clients=%d", connfd, http_conn::m_user_count.load());
                    close(connfd);
                    continue;
                }
                // 将新的客户的数据初始化， 放入数组中
                if (!users[connfd]) users[connfd].reset(new (std::nothrow) http_conn);
                if (!users[connfd]) { close(connfd); continue; }
                users[connfd]->init( connfd, client_address);
            
            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                LOG_DEBUG("epoll", "event=disconnect fd=%d events=%u", sockfd, events[i].events);
                // 对方异常断开或错误等事件
                if (sockfd < MAX_FD && users[sockfd]) users[sockfd]->close_conn();

            } else if(events[i].events & EPOLLIN) {
                LOG_TRACE("epoll", "event=readable fd=%d events=%u", sockfd, events[i].events);
                // 一次性把全部数据读完
                if (sockfd >= MAX_FD || !users[sockfd]) continue;
                if(users[sockfd]->read()) {
                    LOG_TRACE("threadpool", "event=submit fd=%d", sockfd);
                    if (!pool->append(users[sockfd].get())) {
                        LOG_WARN("threadpool", "event=submit_failed fd=%d reason=stopped_or_full", sockfd);
                        users[sockfd]->close_conn();
                    }
                } else {
                    users[sockfd]->close_conn();
                }

            }  else if( events[i].events & EPOLLOUT ) {
                LOG_TRACE("epoll", "event=writable fd=%d events=%u", sockfd, events[i].events);
                // 一次性把全部数据写完
                if (sockfd >= MAX_FD || !users[sockfd]) continue;
                if( !users[sockfd]->write() ) {
                    users[sockfd]->close_conn();
                }

            }
        }
    }
    
    LOG_INFO("main", "event=server_stop listen_fd=%d", listenfd);
    close( listenfd );
    // 先等待工作线程退出；它们结束前仍可能调用 modfd()。
    delete pool;
    delete [] users;
    close( epollfd );
    return 0;
}
