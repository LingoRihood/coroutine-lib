#include "scheduler.h"

static bool debug = false;

namespace sylar {
static thread_local Scheduler* t_scheduler = nullptr;

Scheduler* Scheduler::GetThis() {
    return t_scheduler;
}

void Scheduler::SetThis() {
    t_scheduler = this;
}

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string& name)
    : m_useCaller(use_caller), m_name(name) {
        //首先判断线程的数量是否大于0，并且调度器的对象是否是空指针，是就调用setThis()进行设置.
        assert(threads > 0 && Scheduler::GetThis() == nullptr);
        
        //设置当前调度器对象
        // public 成员：可以被任何位置的代码访问。
        // protected 成员：只能被本类成员函数、友元函数、或派生类成员函数访问。
        // private 成员：只能被本类的成员函数或友元函数访问。
        // 由于public成员函数是类的成员函数，因此可以自由调用类的protected成员函数。
        // 这里的 this 就是当前正在构造的 Scheduler 对象
        SetThis();

        //设置当前线程的名称为调度器的名称 t_thread_name = name;
        Thread::SetName(m_name);

        // 使用主线程当作工作线程，创建协程的主要原因是为了实现更高效的任务调度和管理
        if(use_caller) {
            //如果user_caller为true，表示当前线程也要作为一个工作线程使用。
            //因为此时作为了工作线程所以线程数量--
            --threads;
            // 创建主协程
            Fiber::GetThis();

            // 创建调度协程
            // false -> 该调度协程退出后将返回主协程
            // reset() 是智能指针的方法，用来释放当前管理的对象，并将新的对象赋给智能指针管理。也就是说，m_schedulerFiber.reset(...) 会释放当前 m_schedulerFiber 管理的 Fiber 对象，并赋给它一个新的 Fiber 对象
            // 实际上是为调度器创建了一个专门的协程来运行调度逻辑。即主线程通过 m_schedulerFiber 执行 Scheduler::run()，开始管理和调度任务。
            m_schedulerFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));

            //设置协程的调度器对象
            // 将新创建的协程设置为调度器协程。
            // t_scheduler_fiber = m_schedulerFiber.get()
            Fiber::SetSchedulerFiber(m_schedulerFiber.get());

            //获取主线程ID
            m_rootThread = Thread::GetThreadId();
            m_threadIds.push_back(m_rootThread);

            std::cout << "m_rootThread: " << m_rootThread<< std::endl;
        }

        //将剩余的线程数量（即总线程数量减去是否使用调用者线程）赋值给 m_threadCount
        m_threadCount = threads;    // 2
        if(debug) {
            std::cout << "Scheduler::Scheduler() success\n";
        }
    }

Scheduler::~Scheduler() {
    //判断调度器是否终止
    assert(stopping() == true);

    //获取调度器的对象
    if(GetThis() == this) {
        //将其设置为nullptr防止悬空指针
        t_scheduler = nullptr;
    }
    if(debug) {
        std::cout << "Scheduler::~Scheduler() success\n";
    }
}

void Scheduler::start() {
    //互斥锁防止共享资源的竞争
    std::lock_guard<std::mutex> lock(m_mutex);

    // 标志表示调度器是否已经处于停止状态。
    //如果调度器退出直接报错打印cerr后面的话
    if(m_stopping) {
        std::cerr << "Scheduler is stopped" << std::endl;
		return;
    }

    //首先线程池数量为空
    // 确保线程池尚未启动（空线程池检查）
    assert(m_threads.empty());

    // 根据线程数量m_threadCount调整线程容器大小
    m_threads.resize(m_threadCount);

    // std::cout << m_threadCount<< std::endl;  2
    
    // size_t 是跨平台、平台相关安全类型
    // 在 32 位系统中，size_t 是 32 位；
    // 在 64 位系统中，size_t 是 64 位；
    for(size_t i = 0; i < m_threadCount; ++i) {
        // this 会自动作为 Scheduler::run 的隐式参数传递进去。
        // Scheduler::run() 会在新的线程中运行，且 this 始终指向调用 run 方法的那个 Scheduler 实例。
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
        m_threadIds.push_back(m_threads[i]->getId());
    }
    if(debug) {
        std::cout << "Scheduler::start() success\n";
    }
}

//作用：调度器的核心，负责从任务队列中取出任务并通过协程执行
void Scheduler::run() {
    //获取当前线程的ID
    int thread_id = Thread::GetThreadId();
    if(debug) {
        std::cout << "Schedule::run() starts in thread: " << thread_id << std::endl;
    }

    //set_hook_enable(true);
    //设置调度器对象即t_scheduler = this;
    SetThis();

    // if(thread_id == m_rootThread) {
    //     std::cout <<"main Thread" << std::endl;
    // }

    // 运行在新创建的线程 -> 需要创建主协程
    if(thread_id != m_rootThread) {
        //如果不是主线程，创建主协程
        //分配了线程的主协程和调度协程
        Fiber::GetThis();
    }

    // 创建空闲协程（idle_fiber）
    //创建空闲协程，std::make_shared 是 C++11 引入的一个函数，用于创建 std::shared_ptr 对象。相比于直接使用 std::shared_ptr 构造函数，std::make_shared 更高效且更安全，因为它在单个内存分配中同时分配了控制块和对象，避免了额外的内存分配和指针操作。
    //子协程
    std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));
    ScheduleTask task;

    while(true) {
        // std::cout<< "run" <<std::endl;

        // 每次循环开始前，清空任务内容，设置标志位tickle_me为false。
        // tickle_me表示是否需要通知其他线程有可执行的任务。
        task.reset();

        //是否唤醒了其他线程进行任务调度
        bool tickle_me = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_tasks.begin();

            // 1 遍历任务队列
            // 当 std::vector 为空时，begin() 和 end() 都会返回指向容器尾部的迭代器，这两个迭代器是相同的。
            while(it != m_tasks.end()) {
                // std::cout << "xx" << std::endl;

                //不能等于当前thread_id,其目的是让任何的线程都可以执行。
                // 这段代码的作用是过滤任务队列中的任务，确保任务只在指定的线程上执行，或者允许任务在任何线程上执行。如果任务被锁定在特定线程上，并且当前线程不是该线程，就跳过该任务。
                if(it->thread != -1 && it->thread != thread_id) {
                    // std::cout <<"thread"<< std::endl;
                    ++it;
                    tickle_me = true;
                    continue;
                }

                // 2 取出任务
                //这里取到任务的线程就直接break所以并没有遍历到队尾
                assert(it->fiber || it->cb);
                task = *it;
                m_tasks.erase(it);
                ++m_activeThreadCount;
                break;
            }
            //确保仍然存在未处理的任务
            tickle_me = tickle_me || (it != m_tasks.end());
        }
        if(tickle_me) {
            //这里虽然写了唤醒但并没有具体的逻辑代码，具体的在io+scheduler
            tickle();
        }

        // 3 执行任务
        // 若任务为已有Fiber：
        if(task.fiber) {
            //resume协程，resume返回时此时任务要么执行完了，要么半路yield了，总之任务完成了，活跃线程-1；
            {
                std::lock_guard<std::mutex> lock(task.fiber->m_mutex);
                if(task.fiber->getState() != Fiber::TERM) {
                    task.fiber->resume();
                }
            }
            //线程完成任务后就不再处于活跃状态，而是进入空闲状态，因此需要将活跃线程计数减一。
            --m_activeThreadCount;
            task.reset();
        } else if(task.cb) {
            // 将回调函数包装成新的Fiber执行（这样可统一协程和回调任务的管理方式）
            std::shared_ptr<Fiber> cb_fiber = std::make_shared<Fiber>(task.cb);
            {
                std::lock_guard<std::mutex> lock(cb_fiber->m_mutex);
                cb_fiber->resume();
            }
            --m_activeThreadCount;
            task.reset();
        } else {
            // 4 无任务 -> 执行空闲协程
            // 系统关闭 -> idle协程将从死循环跳出并结束 -> 此时的idle协程状态为TERM -> 再次进入将跳出循环并退出run()
            if(idle_fiber->getState() == Fiber::TERM) {
                //如果调度器没有调度任务，那么idle协程回不断的resume/yield,不会结束进入一个忙等待，如果idele协程结束了，一定是调度器停止了，直到有任务才执行上面的if/else，在这里idle_fiber就是不断的和主协程进行交互的子协程
                if(debug) {
                    std::cout << "Schedule::run() ends in thread: " << thread_id << std::endl;
                }
                break;
            }
            ++m_idleThreadCount;
            idle_fiber->resume();

            // std::cout<< "111"<< std::endl;
            
            --m_idleThreadCount;
            // std::cout << "m_idleThreadCount"<< std::endl;
        }
    }
}

// 用于安全地停止调度器(Scheduler)，它会通知所有线程和协程终止运行，等待它们完成后才退出。
void Scheduler::stop() {
    if(debug) {
        std::cout << "Schedule::stop() starts in thread: " << Thread::GetThreadId() << std::endl;
    }

    if(stopping()) {
        // std::cout << "stopping!!!"<< std::endl;
        return;
    }

    m_stopping = true;

    if(m_useCaller) {
        assert(GetThis() == this);
    } else {
        assert(GetThis() != this);
    }

    // 唤醒所有线程（tickle机制）
    for(size_t i = 0; i < m_threadCount; ++i) {
        // std::cout << "m_threadCount: "<< m_threadCount<< std::endl;
        tickle();
    }

    // 唤醒调度协程（schedulerFiber）
    if(m_schedulerFiber) {
        tickle();
    }

    // 恢复执行调度协程（schedulerFiber）
    if(m_schedulerFiber) {
        m_schedulerFiber->resume();
        if(debug) {
            std::cout << "m_schedulerFiber ends in thread:" << Thread::GetThreadId() << std::endl;
        }
    }

    // 将线程列表转移到临时向量
    std::vector<std::shared_ptr<Thread>> thrs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 使用swap效率更高（避免逐个拷贝shared_ptr），并且确保线程安全地一次性转移所有线程对象。
        // 它仅仅交换两个vector对象的内部指针，通常是O(1)操作，不涉及元素的拷贝
        // m_threads ────> [Thread1, Thread2, Thread3, ...]
        // thrs ────────> []
        thrs.swap(m_threads);
        // m_threads ────> []
        // thrs ────────> [Thread1, Thread2, Thread3, ...]
    }

    for(auto& i: thrs) {
        i->join();
    }
    if(debug) {
        std::cout << "Schedule::stop() ends in thread:" << Thread::GetThreadId() << std::endl;
    }
}

void Scheduler::tickle() {
    // std::cout << "xd" << std::endl;
}

void Scheduler::idle() {
    // std::cout << "xd" << std::endl;

    // 依靠stopping()函数进行检测是否有任务处理
    while(!stopping()) {
        if(debug) {
            std::cout << "Scheduler::idle(), sleeping in thread: " << Thread::GetThreadId() << std::endl;	
        }
        //降低空闲协程在无任务时对cpu占用率，避免空转浪费资源
        sleep(1);
        Fiber::GetThis()->yield();

        // std::cout << "idle" <<std::endl;
    }
}

bool Scheduler::stopping() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // std::cout << "m_stopping: "<< std::boolalpha<< m_stopping<< std::endl;
    // std::cout << "m_activeThreadCount: "<< m_activeThreadCount<< std::endl;
    return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
}

}
