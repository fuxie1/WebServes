#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<pthread.h>
#include<list>
#include"locker.h"
#include<exception>
#include<cstdio>

//线程池类， 定义成模板类是为了代码的复用, 模板参数T是任务类
template<typename T>
class threadPool {
public:
    threadPool(int thread_number = 8, int max_requests = 10000);
    ~threadPool();
    bool append(T* request);    //添加任务方法
    void run();                 //启动线程池

private:
    static void* worker(void* arg); //静态成员函数

private:
    //线程的数量
    int m_thread_number;

    //线程池数组， 大小为m_thread_number
    pthread_t * m_threads;

    //请求队列中最多允许的， 等待处理的请求数量
    int m_max_requests;

    //请求队列
    std::list<T*> m_workQueue;

    //互斥锁
    locker m_queueLocker;

    //信号量，用来判断是否有任务需要处理
    sem m_queueStat;

    //是否结束线程
    bool m_stop;
};

//构造函数实现
template<typename T> 
threadPool<T>::threadPool(int thread_number, int max_requests) : m_thread_number(thread_number), 
                m_max_requests(max_requests), m_stop(false), m_threads(NULL) {

    if ((thread_number <= 0) || (max_requests <= 0)) {
        throw std::exception();
    }
    //创建线程数组
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads) {
        throw std::exception();
    }

    //创建thread_number个线程，并将其设置为线程脱离
    for (int i = 0; i < thread_number; i++) {
        printf("create the %dth thread\n", i);

        //创建线程， 回调函数worker必须为静态函数
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }

        //设置线程脱离
        if (pthread_detach(m_threads[i])) {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

//析构函数实现
template<typename T> 
threadPool<T>::~threadPool() {
    delete[] m_threads;
    m_stop = true;
}

//添加请求任务函数
template<typename T> 
bool threadPool<T>::append(T * request) {

    m_queueLocker.lock();   //上锁

    //超出最大值了，无法添加任务
    if (m_workQueue.size() > m_max_requests) {  
        m_queueLocker.unlock();
        return false;
    }

    m_workQueue.push_back(request); //添加一个任务到队尾
    m_queueLocker.unlock();         //解锁
    m_queueStat.post();             //增加一个信号量
    return true;
}

//静态成员函数worker实现
template<typename T> 
void* threadPool<T>::worker(void* arg) {
    threadPool* pool = (threadPool*)arg;    //参数arg就是传入的this指针指向的对象，即threadPool对象
    pool->run();        //启动线程池
    return pool;
}

template<typename T> 
void threadPool<T>::run() {
    while (!m_stop) {
        m_queueStat.wait();     //信号量>0，信号量减一且取任务， 否则阻塞等待
        m_queueLocker.lock();   //上锁，操作请求队列
        if (m_workQueue.empty()) {  //队列是否为空
            m_queueLocker.unlock(); //是就解锁
            continue;               //继续循环判断是否来任务了。
        }

        T* request = m_workQueue.front();    //取出第一个任务
        m_workQueue.pop_front();             //从队列里删除已经取出的任务
        m_queueLocker.unlock();              //释放锁

        if (!request) {
            continue;       //未获取到任务，继续循环
        }

        request->process(); //获取到了进行任务处理

    }
}
#endif