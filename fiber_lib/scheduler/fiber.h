#ifndef _COROUTINE_H_
#define _COROUTINE_H_

#include <iostream>     
#include <memory>       
#include <atomic>       
#include <functional>   
#include <cassert>      
#include <ucontext.h>   
#include <unistd.h>
#include <mutex>

namespace sylar {
// 用于帮助一个对象在自己内部创建指向自己的 shared_ptr。这样做可以避免对象的生命周期管理问题，确保它在有多个共享指针引用时正确地被销毁。
// 在对象的成员函数中获取指向该对象的 shared_ptr，而不必显式地创建一个新的 shared_ptr。
// 防止对象被提前销毁，确保在使用它的其他地方仍然能维持有效的引用。
class Fiber: public std::enable_shared_from_this<Fiber> {
public:
    // 协程状态
    enum State {
        READY,
        RUNNING,
        TERM
    };
private:
    // 仅由GetThis()调用 -> 私有 -> 创建主协程  
    Fiber();

public:
    Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true);
    ~Fiber();

    // 重用一个协程
    void reset(std::function<void()> cb);

    // 任务线程恢复执行
    void resume();

    // 任务线程让出执行权
    void yield();

    uint64_t getId() const {
        return m_id;
    }

    State getState() const {
        return m_state;
    }

public:
    // 设置当前运行的协程
    static void SetThis(Fiber *f);

    // 得到当前运行的协程 
    static std::shared_ptr<Fiber> GetThis();

    // 设置调度协程（默认为主协程）
    static void SetSchedulerFiber(Fiber* f);

    // 得到当前运行的协程id
    static uint64_t GetFiberId();

    // 协程函数
    static void MainFunc();

private:
    // id
    uint64_t m_id = 0;
    // 栈大小
    uint32_t m_stacksize = 0;
    // 协程状态
    State m_state = READY;
    // 协程上下文
    ucontext_t m_ctx;
    // 协程栈指针
    void* m_stack = nullptr;
    // 协程函数
    std::function<void()> m_cb;
    // 是否让出执行权交给调度协程
    bool m_runInScheduler;

public:
    std::mutex m_mutex;
};
}
#endif