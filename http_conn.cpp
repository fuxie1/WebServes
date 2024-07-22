#include"http_conn.h"


int http_conn::m_epollfd = -1;  //所有的socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态
int http_conn::m_user_count = 0;    //统计用户的数量
int http_conn::m_request_cnt = 0; 
sort_timer_lst http_conn::m_timer_lst;

//定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file";
const char* error_404_title = "Not Found";
const char* error_404_form = "The request file was not found";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the request file";

//网站的根目录
const char* doc_root = "/home/yf415/webServer/resources";

//设置文件描述符非阻塞
int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//添加需要监听的文件描述符到epoll中
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot) {
        //防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //设置文件描述符非阻塞
    setnonblocking(fd);
}

//从epoll中删除监听的文件描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//修改文件描述符,重置socket上的EOPLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发。
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//初始化新接收的连接，外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr) {
    m_sockfd = sockfd;
    m_address = addr;

    //端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //将新连接添加到epoll中进行监听
    addfd(m_epollfd, sockfd, true);
    m_user_count++; //总用户数+1

    char ip[16] = "";
    const char* str = inet_ntop(AF_INET, &addr.sin_addr.s_addr, ip, sizeof(ip));
    EMlog(LOGLEVEL_INFO, "The No.%d user. sock_fd = %d, ip = %s.\n", m_user_count, sockfd, str);

    init();     //其余信息初始化

    //创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
    util_timer* new_timer = new util_timer;
    new_timer->user_data = this;
    time_t cur_time = time(NULL);
    new_timer->expire = cur_time + 3 * TIMESLOT;
    this->timer = new_timer;
    m_timer_lst.add_timer(new_timer);
}

//初始化连接其余的信息
void http_conn::init() {

    bytes_to_send = 0;      //要发送的字节数
    bytes_have_send = 0;    //已发送的字节数

    m_check_state = CHECK_STATE_REQUESTLINE;        //初始化状态为解析请求首行
    m_linger = false;                               //是否保持HTTP长连接，keep-alive功能，默认不保持
    m_method = GET;             //默认请求方式为GET
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_check_index = 0;
    m_start_line = 0;
    m_read_idx = 0;
    m_write_idx = 0;


    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);   

}   

//关闭连接
void http_conn::close_conn() {
    if (m_sockfd != -1) {
        m_user_count--; //关闭一个连接，总连接数减1
        EMlog(LOGLEVEL_INFO, "closing fd: %d, rest user num :%d\n", m_sockfd, m_user_count);
        removefd(m_epollfd, m_sockfd);  //移除epoll检测，关闭套接字
        m_sockfd = -1;
    }
}

//非阻塞读，循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    //更新超时时间
    if (timer) {
        time_t cur_time = time(NULL);
        timer->expire = cur_time + 3 * TIMESLOT;
        m_timer_lst.adjust_timer(timer);
    }

    //printf("一次性读完\n");
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }

    //读取到的字节
    int bytes_read = 0;
    while (true) {
        // 从m_read_buf + m_read_idx索引处开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                //没有数据
                break;
            }
            return false;
        }
        else if (bytes_read == 0) {
            //对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;   //索引移动
    }
    //printf("读取到了数据: %s\n", m_read_buf);
    m_request_cnt++;
    EMlog(LOGLEVEL_INFO, "sock_fd = %d read done. request cnt = %d\n", m_sockfd, m_request_cnt);    // 全部读取完毕
    return true;
}

//主状态机，解析HTTP请求
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char* text = 0;

    while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) 
            || ((line_status = parse_line()) == LINE_OK)) {  //一行一行的解析
            //解析到了一行完整的数据， 或者解析到了请求体，也是完成的数据

            //获取一行数据
            text = get_line();

            m_start_line = m_check_index;   //更细下一行的起始位置

            EMlog(LOGLEVEL_DEBUG, ">>>>>> %s\n", text);

            printf("got 1 http line : %s\n", text);

            switch(m_check_state) {
                case CHECK_STATE_REQUESTLINE:
                {
                    ret = parse_request_line(text);
                    if (ret == BAD_REQUEST) {
                        return BAD_REQUEST;
                    }
                    break;
                }
                case CHECK_STATE_HEADER:
                {
                    ret = parse_headers(text);
                    if (ret == BAD_REQUEST) {
                        return BAD_REQUEST;
                    }
                    else if (ret == GET_REQUEST) {
                        return do_request();    //解析具体的请求信息
                    }
                    break;
                }
                case CHECK_STATE_CONTENT:
                {
                    ret = parse_content(text);
                    if (ret == GET_REQUEST) {
                        return do_request();
                    }
                    line_status = LINE_OPEN;
                    break;
                }
                default:
                {
                    return INTERNAL_ERROR;
                }
            }
            
        }
        return NO_REQUEST;
}   
//解析HTTP请求首行，获取请求方法， 目标URL， HTTP版本                     
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");   //判断空格字符" "和"\t"哪个在text中先出现，返回先出现字符的索引
    if (!m_url) {
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';        // 置位空字符，字符串结束符
    char* method = text;
    //判断是否是get请求方法，忽略大小写比较
    if (strcasecmp(method, "GET") == 0) m_method = GET;
    else return BAD_REQUEST;

    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    // if (strcasecmp(m_version, "HTTP/1.1") != 0) {
    //     return BAD_REQUEST;
    // }
    /**
     * http://192.168.117.128:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER; //主状态机的状态 变成 检查 请求头
    return NO_REQUEST;  


}
//解析HTTP请求头      
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
    //遇到空行，表示头部字段解析完毕
    if (text[0] == '\0') {
        //如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体
        //状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        //处理Connection头部字段， Connection: keep-alive
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        //处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0) {
        //处理Host头部字段
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else {
        #ifdef COUT_OPEN
            EMlog(LOGLEVEL_DEBUG,"oop! unknow header: %s\n", text );
        #endif  
    }
    return NO_REQUEST;
} 
//解析HTTP请求体，没有真正的解析，只是判断是否被完整读入           
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
    if (m_read_idx >= (m_content_length + m_check_index)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}           

//解析具体某行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for (; m_check_index < m_read_idx; m_check_index++) {   //检查的索引小于读到的索引
        temp = m_read_buf[m_check_index];           //遍历缓冲区字符
        if (temp == '\r') {
            if (m_check_index + 1 == m_read_idx) {  //回车符是已经读到的最后一个字符，表示行数据尚不完整
                return LINE_OPEN;
            }
            else if (m_read_buf[m_check_index + 1] == '\n') {   //当前检查到\r\n
                m_read_buf[m_check_index++] = '\0';             //\r 变 \0 index++
                m_read_buf[m_check_index++] = '\0';             //\n 变 \0 index++，到下一行的起始位置
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n') {
            if ((m_check_index > 1) && (m_read_buf[m_check_index - 1] == '\r')) {   //上一次读取的数据行不完整，刚好\r\n在不容数据的结尾和开头的情况
                m_read_buf[m_check_index - 1] = '\0';       //\r 变 \0 index++
                m_read_buf[m_check_index++] = '\0';         //\n 变 \0 index++，到下一行的起始位置
                return LINE_OK;
            }
            return LINE_BAD;
        }
        
    }
    return LINE_OPEN;       //没有到结束符，数据尚不完整
}                        

// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request() {
    // "/home/yf415/webServer/resources"
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);  //拼接目录
    //获取m_real_file文件的相关的状态信息，-1失败，0成功
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_REQUEST;
    }

    //判断访问权限
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    //判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    //以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    //创建内存映射
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);          //关闭打开的网站资源文件
    return FILE_REQUEST;
}

//对内存映射区执行munmap操作
void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:      //请求服务器文件
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            //对两块内存进行封装
            m_iv[ 0 ].iov_base = m_write_buf;   //起始地址
            m_iv[ 0 ].iov_len = m_write_idx;    //长度
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;                     //内存块数

            bytes_to_send = m_write_idx + m_file_stat.st_size;  //响应头的大小 + 文件的大小

            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
//写回HTTP响应，非阻塞写
bool http_conn::write() {
    int temp = 0;

    //更新超时时间
    if (timer) {
        time_t cur_time = time(NULL);
        timer->expire = cur_time + 3 * TIMESLOT;
        m_timer_lst.adjust_timer(timer);
    }

    EMlog(LOGLEVEL_INFO, "sock_fd = %d writing %d bytes. request cnt = %d\n", m_sockfd, bytes_to_send, m_request_cnt);

    if (bytes_to_send == 0) {
        //如果即将要发送的字符为0，这一次响应结束
        modfd(m_epollfd, m_sockfd, EPOLLIN);    //修改监听连接为读
        init();         //
        return true;
    }
    while (1) {
        //分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1) {
            //如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间服务器无法立即接收
            //同一客户的下一个请求，但可以保证里拦截的完整性。
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();        //释放内存映射空间
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len) {   //发完头部了
            m_iv[0].iov_len = 0;                    //更新两个发送内存块的信息
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);    //已经发了部分的响应体数据
            m_iv[1].iov_len = bytes_to_send;
        }
        else {      //还未发完头部
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }
    if (bytes_to_send <= 0) {
            // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

//往写缓冲区中写入待发送的数据
bool http_conn::add_response(const char* format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE) {     //写缓冲区满了
        return false;
    }
    va_list arg_list;                   //可变参数，格式化文本
    va_start(arg_list, format);         //添加文本到写缓冲区m_write_buf中
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        return false;                   //没写完，已经满了
    }
    m_write_idx += len;                 //更新下次写数据的起始位置
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
    return true;
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
    return add_response("Content-Type:%s\r\n", "text/html");
}

//处理客户端请求，解析报文并封装客户端需要的数据
//由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process() {

    //解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);    //继续监听事件
        return;
    }

    //生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
        if (timer) {
            m_timer_lst.del_timer(timer);   //移除其对应的定时器
        }
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);   //重置EPOLLONESHOT
} 
