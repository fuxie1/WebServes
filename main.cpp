#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<errno.h>
#include<fcntl.h>
#include<sys/epoll.h>
#include"locker.h"
#include"threadPool.h"
#include<signal.h>
#include"http_conn.h"
#include<assert.h>
#include"lst_timer.h"
#include"log.h"

#define MAX_FD 65536    //最大文件描述符个数
#define MAX_EVENT_NUMBER 10000    //一次监听的最大的事件数量

static int pipefd[2];           // 管道文件描述符 0为读，1为写

//添加信号捕捉
void addsig(int sig, void(handler)(int)) {
    struct sigaction sa;            //注册信号的参数
    memset(&sa, '\0', sizeof(sa));  //清空， 置为0
    sa.sa_handler = handler;        //回调函数
    sigfillset(&sa.sa_mask);        //将临时阻塞信号集中的所有的标志位置为1，即都阻塞
    assert( sigaction(sig, &sa, NULL) != -1 );       //设置信号捕捉sig信号值
}

// 向管道写数据的信号捕捉回调函数
void sig_to_pipe(int sig){
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

//添加文件描述符到epoll中 (声明成外部函数)
extern void addfd(int epollfd, int fd, bool one_shot);

//从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);

//在epoll中修改文件描述符
extern void modfd(int epollfd, int fd, int ev);

// 文件描述符设置非阻塞操作
extern void setnonblocking(int fd);

int main(int argc, char* argv[]) {

    //传入参数个数必须大于1
    if (argc <= 1) {    
        printf("按照如下格式运行: %s port_number\n", basename(argv[0]));
        return 1;
    }

    //获取端口号
    int port = atoi(argv[1]);

    //对SIGPIE信号进行处理
    addsig(SIGPIPE, SIG_IGN);   //遇到SIGPIPE信号忽略该信号

    //创建线程池，初始化线程池
    threadPool<http_conn> * pool = NULL;
    try {
        pool = new threadPool<http_conn>;
    }
    catch(...) {
        exit(-1);
    }

    //服务端
    //创建socket
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {
        perror("socket\n");
        exit(-1);
    }

    //设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    int ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    if (ret == -1) {
        perror("bind\n");
        exit(-1);
    }

    //监听
    ret = listen(listenfd, 5);
    if (ret == -1) {
        perror("listen\n");
        exit(-1);
    }

    //创建epoll对象， 事件数组，添加监听文件描述符
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    //将监听的文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false);

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1] );               // 写管道非阻塞
    addfd(epollfd, pipefd[0], false ); // epoll检测读管道

    // 设置信号处理函数
    addsig(SIGALRM, sig_to_pipe);   // 定时器信号
    addsig(SIGTERM, sig_to_pipe);   // SIGTERM 关闭服务器
    bool stop_server = false;       // 关闭服务器标志位

    //创建一个数组用于保存所有的客户端信息
    http_conn * users = new http_conn[MAX_FD];
    http_conn::m_epollfd = epollfd;     //静态成员，类共享

    bool timeout = false;   // 定时器周期已到
    alarm(TIMESLOT);        // 定时产生SIGALRM信号

    //循环检测事件发生
    while(true) {
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);    //阻塞，返回事件数量
        if ((num < 0) && (errno != EINTR)) {
            printf("epoll failure\n");
            break;
        }

        //循环遍历
        for (int i = 0; i < num; i++) {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd) {       //监听文件描述符有事件响应
                //有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);
                if (connfd < 0) {
                    printf("errno is : %d\n", errno);
                }

                if (http_conn::m_user_count >= MAX_FD) {
                    //目前连接数满了。
                    //关闭这个连接
                    close(connfd);
                    continue;
                }
                //将新的客户的数据初始化，放入数组中
                users[connfd].init(connfd, client_address);
                // 当listen_fd也注册了ONESHOT事件时(addfd)，
                // 接受了新的连接后需要重置socket上EPOLLONESHOT事件，确保下次可读时，EPOLLIN 事件被触发
                // modfd(epoll_fd, listen_fd, EPOLLIN);
            }
            //读管道有数据，SIGALRM 或 SIGTERM信号触发
            else if (sockfd == pipefd[0] && (events[i].events & EPOLLIN)) {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) {
                    continue;
                }
                else if (ret == 0) {
                    continue;
                }
                else {
                    for (int i = 0; i < ret; i++) {
                        switch (signals[i]) {
                            case SIGALRM:
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            case SIGTERM:
                                stop_server = true;
                        }
                    }
                }
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                //对方异常断开或错误的事件
                EMlog(LOGLEVEL_DEBUG,"-------EPOLLRDHUP | EPOLLHUP | EPOLLERR--------\n");
                users[sockfd].close_conn();
                http_conn::m_timer_lst.del_timer(users[sockfd].timer);  //移除其对应的定时器
            }
            else if (events[i].events & EPOLLIN) {
                EMlog(LOGLEVEL_DEBUG,"-------EPOLLIN-------\n\n");
                if (users[sockfd].read()) {     //主进程一次性把读缓冲区所有数据都读完
                    // 加入到线程池队列中，数组指针 + 偏移 &users[sock_fd]
                    pool->append(users + sockfd);
                }
                else {
                    users[sockfd].close_conn();
                    http_conn::m_timer_lst.del_timer(users[sockfd].timer);  // 移除其对应的定时器
                }
            }
            else if (events[i].events & EPOLLOUT) {
                EMlog(LOGLEVEL_DEBUG, "-------EPOLLOUT--------\n\n");
                if (!users[sockfd].write()) {       //主进程一次性写完所有数据
                    users[sockfd].close_conn();     //写入失败
                    http_conn::m_timer_lst.del_timer(users[sockfd].timer);  // 移除其对应的定时器
                }
            }
        }
        // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if (timeout) {
            //处理定时任务，实际上就是调用tick()函数
            http_conn::m_timer_lst.tick();
            //因为一次alarm调用只会引起一次SIGALARM信号，所以要重新定时，以不断触发SIGALARM信号。
            alarm(TIMESLOT);
            timeout = false;        //重置timeout
        }
    }

    close(epollfd);
    close(listenfd);
    close(pipefd[0]);
    close(pipefd[1]);
    delete[] users;
    delete pool;


    return 0;
}