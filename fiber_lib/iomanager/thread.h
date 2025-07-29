#ifndef _THREAD_H_
#define _THREAD_H_

#include <mutex>
#include <condition_variable>
#include <functional>
#include <string>

namespace sylar {
// 用于线程方法间的同步
class Semaphore {
private:
    std::mutex mtx;
    // 条件变量用于在线程等待某个条件时进行同步。
    std::condition_variable cv;
    int count;
public:
    // 信号量初始化为0
    explicit Semaphore(int count_ = 0): count(count_) {}

    // P 操作（也称为 Wait 或 Decrement）
    void wait() {
        std::unique_lock<std::mutex> lock(mtx);

        // 使用while循环而非if条件，是为了避免虚假唤醒（spurious wakeup）。条件变量可能偶尔出现意外唤醒，此时需要重新检查条件。
        // 虚假唤醒（Spurious wakeup）：
        // 条件变量的wait()方法可能会因系统或实现原因，在未调用notify的情况下被意外唤醒，因此应在循环中反复判断等待条件。
        // cv.wait(lock, []{ return ready; }); // 推荐写法，自动循环检查条件
        // 下面这个是手动循环
        while(count == 0) {
            // 当 count == 0 时，线程会等待条件变量，并且在适当的时候（即信号量值大于0时）被唤醒。
            // 线程在条件变量上等待，释放锁并阻塞
            // 条件变量的wait(lock)相当于原子地完成了：
            // lock.unlock(); // 释放锁
            // // 阻塞，等待唤醒
            // lock.lock();   // 醒来后重新获取锁
            cv.wait(lock);
        }
        count--;
    }

    // V 操作（也称为 Signal 或 Increment）
    // 当 lock 变量生命周期结束（即 signal() 函数结束）时，它的析构函数会自动调用 mtx.unlock()，释放互斥锁。
    void signal() {
        // 使用 std::unique_lock 对 mtx 这把互斥锁加锁（lock），并在 lock 生命周期结束时自动释放（unlock）它。
        // 会自动调用 mtx.lock() 获取互斥锁
        std::unique_lock<std::mutex> lock(mtx);
        count++;
        // signal
        // 通知一个等待的线程资源已经可用
        // notify_one 只会唤醒一个线程，如果多个线程在等待，可以使用 cv.notify_all() 唤醒所有等待线程。
        cv.notify_one();
    }   // 4. 离开作用域，lock 被销毁 → 自动解锁
};

// 一共两种线程: 1 由系统自动创建的主线程 2 由Thread类创建的线程 
class Thread {
public:
    Thread(std::function<void()> cb, const std::string& name);
    
    ~Thread();

    // 返回线程的 ID（m_id）。这是一个唯一标识线程的 ID，通常由操作系统分配。
    pid_t getId() const {
        return m_id;
    }

    // 返回线程的名称（m_name）。这个名称对于调试和日志记录非常有用。
    const std::string& getName() const {
        return m_name;
    }

    // 阻塞当前线程，直到与之相关联的线程执行完毕。通常在这个函数内部会调用 pthread_join() 来等待线程结束。
    void join();

public:
    // 获取系统分配的线程id
    static pid_t GetThreadId();

    // 获取当前所在线程
    static Thread* GetThis();

    // 获取当前线程的名字
    static const std::string& GetName();

    // 设置当前线程的名字
    static void SetName(const std::string& name);

private:
    // 线程函数
    static void* run(void* arg);
private:
    pid_t m_id = -1;
    pthread_t m_thread = 0;

    // 线程需要运行的函数
    std::function<void()> m_cb;
    std::string m_name;

    Semaphore m_semaphore;
};
}
#endif