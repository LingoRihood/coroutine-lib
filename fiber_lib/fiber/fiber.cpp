#include "fiber.h"

static bool debug = true;

namespace sylar {
// 当前线程上的协程控制信息

// 正在运行的协程
static thread_local Fiber* t_fiber = nullptr;
// 主协程
static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;
// 调度协程
static thread_local Fiber* t_scheduler_fiber = nullptr;

// 协程id
static std::atomic<uint64_t> s_fiber_id{0};

// 协程计数器
static std::atomic<uint64_t> s_fiber_count{0};

void Fiber::SetThis(Fiber* f) {
    t_fiber = f;
}

// 首先运行该函数创建主协程
// 获取当前线程正在运行的协程（Fiber）实例的 shared_ptr
std::shared_ptr<Fiber> Fiber::GetThis() {
    if(t_fiber) {
        return t_fiber->shared_from_this();
    }

    // new Fiber() 会调用私有构造函数（只允许在此内部创建），表示主协程专属构造路径。
    std::shared_ptr<Fiber> main_fiber(new Fiber());
    t_thread_fiber = main_fiber;

    // 除非主动设置 主协程默认为调度协程
    // get() 函数是C++标准库中智能指针（如 std::shared_ptr 或 std::unique_ptr）的成员函数，用于获取指向对象的原始指针
    t_scheduler_fiber = main_fiber.get();

    assert(t_fiber == main_fiber.get());
    return t_fiber->shared_from_this();
}

void Fiber::SetSchedulerFiber(Fiber* f) {
    t_scheduler_fiber = f;
}

uint64_t Fiber::GetFiberId() {
    // 正常情况：返回当前协程的 ID；
    if(t_fiber) {
        return t_fiber->getId();
    }
    // 异常情况（尚未初始化协程系统）：返回 -1，即 uint64_t 类型的最大值 18446744073709551615，表示无效协程。
    return (uint64_t)-1;
}

Fiber::Fiber() {
    SetThis(this);
    m_state = RUNNING;

    // 成功时，m_ctx 就保存了当前线程主函数的执行状态；
    if(getcontext(&m_ctx)) {
        std::cerr<< "Fiber() failed\n";
        pthread_exit(NULL);
    }

    // 为当前协程分配一个唯一 ID；
    // s_fiber_id 是一个静态变量（全局递增），确保每个协程都有唯一标识；
    m_id = s_fiber_id++;

    // s_fiber_count 是静态变量，表示当前存活的 Fiber 总数量；
    // 便于监控或调试内存泄漏（是否有 Fiber 没有释放）；
    s_fiber_count++;
    if(debug) {
        std::cout << "Fiber(): main id = " << m_id << std::endl;
    }
}

Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler)
    : m_cb(cb), m_runInScheduler(run_in_scheduler) {
        m_state = READY;

        // 分配协程栈空间
        // 协程栈的大小，单位为字节。如果用户没有指定（传入0），则使用默认大小128000字节（约128KB）
        m_stacksize = stacksize ? stacksize: 128000;
        m_stack = malloc(m_stacksize);

        // 调用getcontext(&m_ctx)，保存当前执行上下文到m_ctx结构体中。
        if(getcontext(&m_ctx)) {
            std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";
		    pthread_exit(NULL);
        }

        m_ctx.uc_link = nullptr;
        // 指定协程上下文的栈空间起始地址。
        m_ctx.uc_stack.ss_sp = m_stack;
        m_ctx.uc_stack.ss_size = m_stacksize;
        // 使用makecontext函数，将m_ctx上下文设置为执行函数Fiber::MainFunc，此时上下文创建完成，当协程首次切换执行时，就会调用Fiber::MainFunc
        makecontext(&m_ctx, &Fiber::MainFunc, 0);

        m_id = s_fiber_id++;
        s_fiber_count++;
        if(debug) {
            std::cout << "Fiber(): child id = " << m_id << std::endl;
        }
    }

Fiber::~Fiber() {
    --s_fiber_count;
    if(m_stack) {
        free(m_stack);
    }

    if(debug) {
        std::cout << "~Fiber(): id = " << m_id << std::endl;	
    }
}

//作用：重置协程的回调函数，并重新设置上下文，使用与将协程从`TERM`状态重置READY
// 用于重置（复用）一个已结束（TERMINATED状态）的协程对象，让它可以再次运行新的任务
// 以避免频繁创建和销毁协程对象带来的性能损失。
void Fiber::reset(std::function<void()> cb) {
    // 确保协程对象已处于终止状态（TERM），且栈空间已分配。
    assert(m_stack != nullptr && m_state == TERM);
    m_state = READY;
    m_cb = cb;

    if(getcontext(&m_ctx)) {
        std::cerr<< "reset() failed\n";
		pthread_exit(NULL);
    }

    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    makecontext(&m_ctx, &Fiber::MainFunc, 0);
}

// 将一个处于准备就绪（READY）状态的协程切换到运行状态（RUNNING）
void Fiber::resume() {
    assert(m_state == READY);

    m_state = RUNNING;

    //这里的切换就相当于非对称协程函数那个当a执行完成后会将执行权交给b
    if(m_runInScheduler) {
        // 将当前协程设为活动协程。
        SetThis(this);
        // 表示当前协程运行在调度器管理之下。
        // 当前的上下文状态会被保存到scheduler协程的上下文中，然后启动或继续目标协程（即本协程，this）的执行。
        if(swapcontext(&(t_scheduler_fiber->m_ctx), &m_ctx)) {
            std::cerr << "resume() to t_scheduler_fiber failed\n";
			pthread_exit(NULL);
        }
    } else {
        // 表示协程直接运行于某个线程上下文，而非调度器。
        // 通常用于简单场景或线程主协程切换。
        SetThis(this);
        if(swapcontext(&(t_thread_fiber->m_ctx), &m_ctx)) {
            std::cerr << "resume() to t_thread_fiber failed\n";
			pthread_exit(NULL);
        }
    }
}

// 协程主动让出执行权，切换回到调度器协程或线程主协程
void Fiber::yield() {
    assert(m_state == RUNNING || m_state == TERM);

    if(m_state != TERM) {
        m_state = READY;
    }

    if(m_runInScheduler) {
        // 协程运行在调度器管理下（m_runInScheduler == true）
        SetThis(t_scheduler_fiber);
        if(swapcontext(&m_ctx, &(t_scheduler_fiber->m_ctx))) {
            std::cerr << "yield() to to t_scheduler_fiber failed\n";
			pthread_exit(NULL);
        }
    } else {
        SetThis(t_thread_fiber.get());
        if(swapcontext(&m_ctx, &(t_thread_fiber->m_ctx))) {
            std::cerr << "yield() to t_thread_fiber failed\n";
			pthread_exit(NULL);
        }
    }
}

// 通过封装协程入口函数，可以实现协程在结束自动执行yield的操作。
void Fiber::MainFunc() {
    // 获取当前协程对象，确保引用计数正常。
    std::shared_ptr<Fiber> curr = GetThis();
    assert(curr != nullptr);

    //真正执行任务的地方
    curr->m_cb();
    //防止悬空引用
    curr->m_cb = nullptr;
    curr->m_state = TERM;

    // 运行完毕 -> 让出执行权
    //获取原始指针，不增加引用计数
    auto raw_ptr = curr.get();
    //引用计数-1，如果此时为0，协程对象销毁
    // reset() 的作用是将 shared_ptr 绑定的对象指针置为 nullptr，并减少该对象的引用计数。如果没有其他 shared_ptr 对象共享该资源，那么对象会被销毁。
    curr.reset();
    raw_ptr->yield();
}
}