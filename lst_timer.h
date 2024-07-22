#ifndef LST_TIMER
#define LST_TIMER

#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>
#include "http_conn.h"
#include "locker.h"

class http_conn; //前向声明

// //用户数据结构
// struct client_data {
//     sockaddr_in address;    //客户端socket地址
//     int sockfd;             //socket文件描述符
//     char buf[BUFFER_SIZE];  //读缓存
//     util_timer* timer;      //定时器
// };

//定时器类
class util_timer {
public:
    util_timer() : prev(nullptr), next(nullptr) {}

public:
    time_t expire;      //任务超时时间，这里使用绝对时间
    http_conn* user_data;
    util_timer* prev;       //指向前一个定时器
    util_timer* next;       //指向后一个定时器
};

//定时器链表类，它是一个升序、双向链表，且带有头节点和尾节点。
class sort_timer_lst {
public:
    sort_timer_lst() : head(nullptr), tail(nullptr) {}

    //链表被销毁时，删除其中所有的定时器
    ~sort_timer_lst() {
        util_timer* tmp = head;
        while (tmp) {
            head = head->next;
            delete tmp;
            tmp = head;
        }
    }

    // 将目标定时器timer添加到链表中
    void add_timer(util_timer* timer);    

    /* 当某个定时任务发生变化时，调整对应的定时器在链表中的位置。这个函数只考虑被调整的定时器的
    超时时间延长的情况，即该定时器需要往链表的尾部移动。*/
    void adjust_timer(util_timer* timer);

    // 将目标定时器 timer 从链表中删除
    void del_timer(util_timer* timer);
    /* SIGALARM 信号每次被触发就在其信号处理函数中执行一次 tick() 函数，以处理链表上到期任务。*/         
    void tick();                      

private:
    /* 一个重载的辅助函数，它被公有的 add_timer 函数和 adjust_timer 函数调用
    该函数表示将目标定时器 timer 添加到节点 lst_head 之后的部分链表中 */
    void add_timer(util_timer* timer, util_timer* lst_head);

private:
    util_timer* head;   //头节点
    util_timer* tail;   //尾节点
};

#endif