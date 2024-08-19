#ifndef LOCKER_H
#define LOCKER_H

#include<pthread.h>
#include<exception>
#include<semaphore.h>

//线程同步机制封装类

//1、互斥锁类
class locker {
public:
    //构造函数创建互斥锁
    locker() {
        if (pthread_mutex_init(&m_mutex, NULL) != 0) {
            throw std::exception(); //抛出异常对象
        }
    }
    //析构销毁锁
    ~locker() {
        pthread_mutex_destroy(&m_mutex);
    }
    //上锁
    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    //解锁
    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    //获取互斥量成员
    pthread_mutex_t * get() {
        return &m_mutex;
    }

private:
    //互斥锁变量--初始化一个互斥锁--上锁--解锁--销毁互斥锁
    pthread_mutex_t m_mutex;
};

//2、条件变量类
class cond {
public:
    cond() {
        if (pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        }
    }

    ~cond() {
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t * mutex) {    //阻塞等待一个条件变量
        return pthread_cond_wait(&m_cond, mutex) == 0;
    }

    bool timewait(pthread_mutex_t * mutex, struct timespec t) {  // 当在指定时间t内有信号传过来时，pthread_cond_timedwait()返回0
        return pthread_cond_timedwait(&m_cond, mutex, &t) == 0;
    }

    bool signal() {   //唤醒 至少一个 阻塞在条件变量上的进程
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast() {  //唤醒 全部 阻塞在条件变量上的进程
        return pthread_cond_broadcast(&m_cond) == 0;
    }
private:
    //条件变量--初始化条件变量--   --销毁条件变量
    pthread_cond_t m_cond;  
};

//3、信号量类
class sem {
public:
    sem() {
        if (sem_init(&m_sem, 0, 0) != 0) {
            throw std::exception();
        }
    }
    //有参构造
    sem(int num) {
        if (sem_init(&m_sem, 0, num) != 0) {
            throw std::exception();
        }
    }
    //析构
    ~sem() {
        sem_destroy(&m_sem);
    }
    //等待信号量
    //该函数用于以原子操作的方式将信号量的值减1。原子操作就是，如果两个线程企图同时给一个信号量加1或减1，它们之间不会互相干扰。
    //等待信号量，如果信号量的值大于0，将信号量的值减1，立即返回。如果信号量的值为0，则线程阻塞。相当于P操作。成功返回0，失败返回-1。m_sem指向的对象是由sem_init调用初始化的信号量。
    bool wait() {
        return sem_wait(&m_sem) == 0;
    }
    //增加信号量
    bool post() {
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};



#endif