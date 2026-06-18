#include "http_conn.h"

#include "shared_buffer.h"
#include "stream_manager.h"



// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
const char* error_503_title = "Service Unavailable";
const char* error_503_form = "Requested media source is unavailable or not ready.\n";

// 网站的根目录（可通过 set_doc_root 运行时设置）
static std::string g_doc_root = "./resources";

// 设置网站根目录（在 main 中通过 /proc/self/exe 自动计算后调用）
void http_conn::set_doc_root(const std::string& path) {
    if (!path.empty() && path[path.size() - 1] == '/') {
        g_doc_root = path.substr(0, path.size() - 1);
    } else {
        g_doc_root = path;
    }
}

const std::string& http_conn::get_doc_root() {
    return g_doc_root;
}

// 设置文件描述符非阻塞
int setnonblocking( int fd ) {
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

// 向epoll中添加需要监听的文件描述符
void addfd( int epollfd, int fd, bool one_shot ) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;
    if(one_shot) 
    {
        // 防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);  
}

// 从epoll中移除监听的文件描述符
void removefd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

// 所有的客户数
std::atomic<int> http_conn::m_user_count(0);
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;
StreamManager* http_conn::m_stream_manager = NULL;

http_conn::http_conn() :
    m_sockfd(-1),
    m_file_address(NULL),
    m_iv_count(0),
    m_bytes_to_send(0),
    m_bytes_have_sent(0),
    m_conn_type(CONN_HTTP),
    m_send_offset(0),
    m_wait_flv_key_frame(false) {
    pthread_mutex_init(&m_send_lock, NULL);
}

http_conn::~http_conn() {
    pthread_mutex_destroy(&m_send_lock);
}

void http_conn::set_stream_manager(StreamManager* manager) {
    m_stream_manager = manager;
}

// 关闭连接
void http_conn::close_conn() {
    pthread_mutex_lock(&m_send_lock);
    int fd = m_sockfd;
    if (fd != -1) {
        m_sockfd = -1;
        m_send_queue.clear();
        m_send_offset = 0;
    }
    pthread_mutex_unlock(&m_send_lock);

    if (fd != -1) {
        if (is_streaming() && m_stream_manager) {
            m_stream_manager->remove_client(this);
        }
        unmap();
        removefd(m_epollfd, fd);
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;
    
    // 端口复用
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    addfd( m_epollfd, sockfd, true );

    // 用户数加1
    m_user_count++; 
    init();
}

// 初始化其他信息
void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    m_file_address = NULL;
    m_iv_count = 0;
    m_bytes_to_send = 0;
    m_bytes_have_sent = 0;
    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, WRITE_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);

    m_conn_type = CONN_HTTP;
    m_wait_flv_key_frame = false;
    pthread_mutex_lock(&m_send_lock);
    m_send_queue.clear();
    m_send_offset = 0;
    pthread_mutex_unlock(&m_send_lock);
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    if( m_read_idx >= READ_BUFFER_SIZE ) {
        return false;
    }
    int bytes_read = 0;
    while(true) {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, 
        READ_BUFFER_SIZE - m_read_idx, 0 );
        if (bytes_read == -1) {
            if( errno == EAGAIN || errno == EWOULDBLOCK ) {
                // 没有数据
                break;
            }
            return false;   
        } else if (bytes_read == 0) {   // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

http_conn::PacketQueueResult http_conn::enqueue_packet(
        const std::shared_ptr<std::vector<unsigned char>>& packet,
        size_t max_queue_depth,
        bool droppable) {
    if (!packet || packet->empty()) {
        return PACKET_DROPPED;
    }

    bool dropped_existing = false;
    pthread_mutex_lock(&m_send_lock);
    if (m_sockfd == -1 || max_queue_depth == 0) {
        pthread_mutex_unlock(&m_send_lock);
        return PACKET_DROPPED;
    }

    while (m_send_queue.size() >= max_queue_depth) {
        size_t first_droppable = (m_send_offset > 0) ? 1 : 0;
        size_t drop_index = m_send_queue.size();
        for (size_t i = first_droppable; i < m_send_queue.size(); ++i) {
            if (m_send_queue[i].droppable) {
                drop_index = i;
                break;
            }
        }

        if (drop_index == m_send_queue.size()) {
            if (droppable) {
                pthread_mutex_unlock(&m_send_lock);
                return PACKET_DROPPED;
            }
            break;
        }

        m_send_queue.erase(m_send_queue.begin() + drop_index);
        if (drop_index == 0) {
            m_send_offset = 0;
        }
        dropped_existing = true;
    }
    m_send_queue.push_back(QueuedPacket(packet, droppable));
    pthread_mutex_unlock(&m_send_lock);

    notify_write();
    return dropped_existing ? PACKET_ENQUEUED_AFTER_DROP : PACKET_ENQUEUED;
}

http_conn::PacketQueueResult http_conn::enqueue_flv_packet(
        const std::shared_ptr<Packet>& packet) {
    if (!packet || !packet->data || packet->data->empty()) {
        return PACKET_DROPPED;
    }

    const size_t max_queue_depth = 100;
    bool dropped_existing = false;

    pthread_mutex_lock(&m_send_lock);
    if (m_sockfd == -1 || m_conn_type != CONN_FLV) {
        pthread_mutex_unlock(&m_send_lock);
        return PACKET_DROPPED;
    }

    if (m_wait_flv_key_frame && !packet->is_config && !packet->is_key_frame) {
        pthread_mutex_unlock(&m_send_lock);
        return PACKET_DROPPED;
    }

    if (m_send_queue.size() >= max_queue_depth) {
        m_send_queue.clear();
        m_send_offset = 0;
        m_wait_flv_key_frame = true;
        dropped_existing = true;
        if (!packet->is_config && !packet->is_key_frame) {
            pthread_mutex_unlock(&m_send_lock);
            return PACKET_DROPPED;
        }
    }

    if (m_wait_flv_key_frame && !packet->is_config && packet->is_key_frame) {
        m_wait_flv_key_frame = false;
    }

    m_send_queue.push_back(QueuedPacket(packet->data, false));
    pthread_mutex_unlock(&m_send_lock);

    notify_write();
    return dropped_existing ? PACKET_ENQUEUED_AFTER_DROP : PACKET_ENQUEUED;
}

void http_conn::notify_write() {
    pthread_mutex_lock(&m_send_lock);
    if (m_sockfd != -1) {
        modfd(m_epollfd, m_sockfd, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
    }
    pthread_mutex_unlock(&m_send_lock);
}

// 解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx ) {
        temp = m_read_buf[ m_checked_idx ];
        if ( temp == '\r' ) {
            if ( ( m_checked_idx + 1 ) == m_read_idx ) {
                return LINE_OPEN;
            } else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' ) {
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if( temp == '\n' )  {
            if( ( m_checked_idx > 1) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) ) {
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析HTTP请求行，获得请求方法，目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符
    char* method = text;
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    // /index.html\0HTTP/1.1
    *m_version++ = '\0';
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }
    /**
     * http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    char* query = strchr(m_url, '?');
    if (query) {
        *query = '\0';
    }
    if (strstr(m_url, "..") != NULL) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头
    return NO_REQUEST;
}

// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {   
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text ) {
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
                || ((line_status = parse_line()) == LINE_OK)) {
            //  解析到一行完整的数据 或者 解析到了请求体， 也是完整的数据
        // 获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;
        printf( "got 1 http line: %s\n", text );

        switch ( m_check_state ) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content( text );
                if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    if (strcmp(m_url, "/mjpeg") == 0) {
        if (!m_stream_manager || !m_stream_manager->is_mjpeg_source_ready()) {
            return SERVICE_UNAVAILABLE;
        }
        return MJPEG_REQUEST;
    }
    if (strcmp(m_url, "/live.flv") == 0 || strcmp(m_url, "/flv") == 0) {
        if (!m_stream_manager) {
            return SERVICE_UNAVAILABLE;
        }
        if (!m_stream_manager->is_flv_source_running()) {
            m_stream_manager->start_flv_source("", 25);
        }
        if (!m_stream_manager->is_flv_source_ready()) {
            return SERVICE_UNAVAILABLE;
        }
        return FLV_REQUEST;
    }
    if (strcmp(m_url, "/api/status") == 0) {
        return STATUS_REQUEST;
    }
    if (strcmp(m_url, "/api/start_flv") == 0) {
        return START_FLV_REQUEST;
    }
    if (strcmp(m_url, "/api/stop_flv") == 0) {
        return STOP_FLV_REQUEST;
    }
    if(strcmp(m_url,"/live")==0){
        std::string path = g_doc_root + "/hls/stream.m3u8";
        strncpy(m_real_file, path.c_str(), FILENAME_LEN - 1);
        m_real_file[FILENAME_LEN - 1] = '\0';
        if (stat(m_real_file, &m_file_stat) < 0) {
            return NO_RESOURCE;
        }
        if ( !(m_file_stat.st_mode & S_IROTH) ) {
            return FORBIDDEN_REQUEST;
        }
        if ( S_ISDIR(m_file_stat.st_mode) ) {
            return BAD_REQUEST;
        }
        int fd = open(m_real_file, O_RDONLY);
        if (fd < 0) {
            return NO_RESOURCE;
        }
        if (m_file_stat.st_size > 0) {
            m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (m_file_address == MAP_FAILED) {
                m_file_address = NULL;
                close(fd);
                return INTERNAL_ERROR;
            }
        } else {
            m_file_address = NULL;
        }
        close(fd);
        return FILE_REQUEST;
    }
    if (strcmp(m_url, "/") == 0) {
        static char index_path[] = "/index.html";
        m_url = index_path;
    }

    // 拼接 doc_root + m_url，doc_root 现在是运行时可配置的相对/绝对路径
    std::string path = g_doc_root + m_url;
    strncpy(m_real_file, path.c_str(), FILENAME_LEN - 1);
    m_real_file[FILENAME_LEN - 1] = '\0';
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    if (fd < 0) {
        return NO_RESOURCE;
    }
    // 创建内存映射
    if (m_file_stat.st_size > 0) {
        m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
        if (m_file_address == MAP_FAILED) {
            m_file_address = NULL;
            close(fd);
            return INTERNAL_ERROR;
        }
    } else {
        m_file_address = NULL;
    }
    close( fd );
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

bool http_conn::write_stream() {
    static uint64_t total_packets_sent = 0;
    while (true) {
        pthread_mutex_lock(&m_send_lock);
        if (m_sockfd == -1) {
            pthread_mutex_unlock(&m_send_lock);
            return false;
        }

        if (m_send_queue.empty()) {
            modfd(m_epollfd, m_sockfd, EPOLLIN | EPOLLRDHUP);
            pthread_mutex_unlock(&m_send_lock);
            return true;
        }

        std::shared_ptr<std::vector<unsigned char>> packet =
            m_send_queue.front().data;
        if (!packet || packet->empty()) {
            m_send_queue.pop_front();
            m_send_offset = 0;
            pthread_mutex_unlock(&m_send_lock);
            continue;
        }

        if (m_send_offset >= packet->size()) {
            m_send_queue.pop_front();
            m_send_offset = 0;
            pthread_mutex_unlock(&m_send_lock);
            continue;
        }

        bool is_new_packet = (m_send_offset == 0);
        if (is_new_packet) {
            total_packets_sent++;
            // 对较大的包（帧数据）打印日志
            if (packet->size() > 256 && (total_packets_sent <= 5 || total_packets_sent % 100 == 0)) {
                printf("[SEND] fd=%d pkt#%llu size=%zu queue=%zu\n",
                       m_sockfd, (unsigned long long)total_packets_sent,
                       packet->size(), m_send_queue.size());
                // 打印包的前32字节（hex）
                printf("[SEND] first32=");
                for (size_t b = 0; b < 32 && b < packet->size(); b++)
                    printf("%02X ", (*packet)[b]);
                printf("\n");
            }
        }

        ssize_t sent = send(m_sockfd,
                            packet->data() + m_send_offset,
                            packet->size() - m_send_offset,
                            0);
        if (sent < 0) {
            if (errno == EINTR) {
                pthread_mutex_unlock(&m_send_lock);
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("[SEND] fd=%d EAGAIN at offset=%zu/%zu pkt#%llu\n",
                       m_sockfd, m_send_offset, packet->size(),
                       (unsigned long long)total_packets_sent);
                modfd(m_epollfd, m_sockfd, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
                pthread_mutex_unlock(&m_send_lock);
                return true;
            }
            pthread_mutex_unlock(&m_send_lock);
            return false;
        }

        if (sent == 0) {
            pthread_mutex_unlock(&m_send_lock);
            return false;
        }

        m_send_offset += static_cast<size_t>(sent);
        if (m_send_offset >= packet->size()) {
            if (packet->size() > 256 && total_packets_sent <= 5) {
                printf("[SEND] fd=%d pkt#%llu COMPLETE size=%zu\n",
                       m_sockfd, (unsigned long long)total_packets_sent,
                       packet->size());
            }
            m_send_queue.pop_front();
            m_send_offset = 0;
        }
        pthread_mutex_unlock(&m_send_lock);
    }
    return true;
}

bool http_conn::build_status_response() {
    if (!m_stream_manager) {
        return false;
    }

    std::string body = m_stream_manager->status_json();
    if (!body.empty() && body[body.size() - 1] == '\n') {
        body.erase(body.size() - 1);
    }
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "{\"http_clients\":%d,\"streams\":",
             m_user_count.load());
    body = std::string(prefix) + body + "}\n";
    return build_json_response(body);
}

bool http_conn::build_json_response(const std::string& body) {
    m_write_idx = 0;
    if (!add_status_line(200, ok_200_title) ||
        !add_response("Content-Type: application/json\r\n") ||
        !add_response("Content-Length: %zu\r\n", body.size()) ||
        !add_linger() ||
        !add_blank_line() ||
        !add_response("%s", body.c_str())) {
        return false;
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    m_bytes_have_sent = 0;
    m_bytes_to_send = static_cast<size_t>(m_write_idx);
    return true;
}

bool http_conn::start_mjpeg_stream() {
    if (!m_stream_manager) {
        fprintf(stderr, "[MJPEG] start failed: no stream_manager, fd=%d\n", m_sockfd);
        return false;
    }

    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    std::shared_ptr<std::vector<unsigned char>> packet =
        std::make_shared<std::vector<unsigned char>>(strlen(header));
    memcpy(packet->data(), header, strlen(header));

    m_conn_type = CONN_MJPEG;
    // MJPEG 队列深度设为 10：响应头(不可丢弃) + 最多9帧 JPEG
    // 队列满时丢弃最旧的 JPEG 帧(可丢弃)，保留响应头和最新帧
    if (enqueue_packet(packet, 10, false) == PACKET_DROPPED) {
        fprintf(stderr, "[MJPEG] start failed: enqueue dropped, fd=%d\n", m_sockfd);
        return false;
    }
    m_stream_manager->add_mjpeg_client(this);
    printf("[MJPEG] stream started, fd=%d, enqueued HTTP header\n", m_sockfd);
    return true;
}

bool http_conn::start_flv_stream() {
    if (!m_stream_manager || !m_stream_manager->is_flv_source_ready()) {
        return false;
    }

    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: video/x-flv\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    std::shared_ptr<std::vector<unsigned char>> http_header =
        std::make_shared<std::vector<unsigned char>>(strlen(header));
    memcpy(http_header->data(), header, strlen(header));

    std::shared_ptr<Packet> flv_header = m_stream_manager->make_flv_header_packet();
    std::shared_ptr<Packet> sequence_header = m_stream_manager->make_flv_sequence_header_packet();
    if (!flv_header || !sequence_header) {
        return false;
    }

    m_conn_type = CONN_FLV;
    m_wait_flv_key_frame = true;

    if (enqueue_packet(http_header, 3, false) == PACKET_DROPPED ||
        enqueue_flv_packet(flv_header) == PACKET_DROPPED ||
        enqueue_flv_packet(sequence_header) == PACKET_DROPPED) {
        return false;
    }

    m_stream_manager->add_flv_client(this);
    return true;
}

// 写HTTP响应
bool http_conn::write()
{
    if (is_streaming()) {
        return write_stream();
    }

    if (m_bytes_to_send == 0) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while (m_bytes_to_send > 0) {
        // 分散写
        ssize_t temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp < 0) {
            if (errno == EINTR) {
                continue;
            }
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }

        if (temp == 0) {
            return false;
        }

        m_bytes_have_sent += static_cast<size_t>(temp);
        m_bytes_to_send -= static_cast<size_t>(temp);

        if (m_bytes_have_sent >= static_cast<size_t>(m_write_idx)) {
            m_iv[0].iov_len = 0;
            if (m_iv_count == 2) {
                if (m_bytes_to_send > 0 && m_file_address) {
                    size_t file_offset = m_bytes_have_sent - static_cast<size_t>(m_write_idx);
                    m_iv[1].iov_base = m_file_address + file_offset;
                } else {
                    m_iv[1].iov_base = m_file_address;
                }
                m_iv[1].iov_len = m_bytes_to_send;
            }
        } else {
            m_iv[0].iov_base = m_write_buf + m_bytes_have_sent;
            m_iv[0].iov_len = static_cast<size_t>(m_write_idx) - m_bytes_have_sent;
        }
    }

    // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
    unmap();
    if (m_linger) {
        init();
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return true;
    }
    return false;
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if (len < 0 || len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) {
    return add_content_length(content_len) &&
           add_content_type() &&
           add_linger() &&
           add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    const char* type = "application/octet-stream";
    const char* dot = strrchr(m_real_file, '.');
    if (dot) {
        if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) {
            type = "text/html";
        } else if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
            type = "image/jpeg";
        } else if (strcasecmp(dot, ".png") == 0) {
            type = "image/png";
        } else if (strcasecmp(dot, ".css") == 0) {
            type = "text/css";
        } else if (strcasecmp(dot, ".js") == 0) {
            type = "application/javascript";
        } else if (strcasecmp(dot, ".m3u8") == 0) {
            type = "application/vnd.apple.mpegurl";
        } else if (strcasecmp(dot, ".ts") == 0) {
            type = "video/MP2T";
        }
    }
    return add_response("Content-Type: %s\r\n", type);
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            if (!add_status_line(500, error_500_title) ||
                !add_headers(strlen(error_500_form)) ||
                !add_content(error_500_form)) {
                return false;
            }
            break;
        case BAD_REQUEST:
            if (!add_status_line(400, error_400_title) ||
                !add_headers(strlen(error_400_form)) ||
                !add_content(error_400_form)) {
                return false;
            }
            break;
        case NO_RESOURCE:
            if (!add_status_line(404, error_404_title) ||
                !add_headers(strlen(error_404_form)) ||
                !add_content(error_404_form)) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            if (!add_status_line(403, error_403_title) ||
                !add_headers(strlen(error_403_form)) ||
                !add_content(error_403_form)) {
                return false;
            }
            break;
        case SERVICE_UNAVAILABLE:
            if (!add_status_line(503, error_503_title) ||
                !add_headers(strlen(error_503_form)) ||
                !add_content(error_503_form)) {
                return false;
            }
            break;
        case FILE_REQUEST:
            if (!add_status_line(200, ok_200_title) ||
                !add_headers(m_file_stat.st_size)) {
                return false;
            }
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            m_bytes_have_sent = 0;
            m_bytes_to_send = static_cast<size_t>(m_write_idx) +
                              static_cast<size_t>(m_file_stat.st_size);
            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    m_bytes_have_sent = 0;
    m_bytes_to_send = static_cast<size_t>(m_write_idx);
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process() {
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST ) {
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }

    if (read_ret == MJPEG_REQUEST) {
        if (!start_mjpeg_stream()) {
            close_conn();
        }
        return;
    }

    if (read_ret == FLV_REQUEST) {
        if (!start_flv_stream()) {
            close_conn();
        }
        return;
    }

    if (read_ret == STATUS_REQUEST) {
        if (!build_status_response()) {
            close_conn();
            return;
        }
        modfd(m_epollfd, m_sockfd, EPOLLOUT);
        return;
    }

    if (read_ret == START_FLV_REQUEST) {
        bool ok = m_stream_manager && m_stream_manager->start_flv_source("", 25);
        std::string body = ok ? "{\"ok\":true,\"flv_running\":true}\n"
                              : "{\"ok\":false,\"error\":\"failed to start flv source\"}\n";
        if (!build_json_response(body)) {
            close_conn();
            return;
        }
        modfd(m_epollfd, m_sockfd, EPOLLOUT);
        return;
    }

    if (read_ret == STOP_FLV_REQUEST) {
        if (m_stream_manager) {
            m_stream_manager->stop_flv_source();
        }
        if (!build_json_response("{\"ok\":true,\"flv_running\":false}\n")) {
            close_conn();
            return;
        }
        modfd(m_epollfd, m_sockfd, EPOLLOUT);
        return;
    }
    
    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
        return;
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}
