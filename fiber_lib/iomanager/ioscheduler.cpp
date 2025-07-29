#include <unistd.h>    
#include <sys/epoll.h> 
#include <fcntl.h>     
#include <cstring>

#include "ioscheduler.h"

static bool debug = true;

namespace sylar {

IOManager* IOManager::GetThis() {
    // dynamic_cast 用于将基类指针转换为派生类指针。在这里，我们将 Scheduler 类的 GetThis() 返回的指针转换为 IOManager*
    // 如果当前调度器实例是 IOManager 类型，转换将成功。如果不是（例如，基类指针指向其他类型），dynamic_cast 将返回 nullptr，这保证了类型安全。
    return dynamic_cast<IOManager*>(Scheduler::GetThis());
}

// 用于获取指定事件类型（如读事件或写事件）对应的事件上下文（EventContext）。FdContext 类中维护了 read 和 write 两个 EventContext，该方法根据传入的 event 类型返回相应的事件上下文
IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(Event event) {
    assert(event == READ || event == WRITE);
    switch(event) {
        // READ 和 WRITE 事件类型会分别返回 read 或 write 对应的 EventContext。
        case READ:
            return read;
        case WRITE:
            return write;
    }
    // 如果传入的 event 既不是 READ 也不是 WRITE，则抛出 std::invalid_argument 异常，表示传入的事件类型无效。
    // 这行代码可以防止传入无效事件类型时造成未定义行为，确保代码的健壮性。
    throw std::invalid_argument("Unsupported event type");
}

void IOManager::FdContext::resetEventContext(EventContext& ctx) {
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}

// no lock
// 用于触发并处理特定文件描述符（fd）上已经发生的事件。
void IOManager::FdContext::triggerEvent(IOManager::Event event) {
    //确保event是中有指定的事件，否则程序中断。
    assert(events & event);

    // delete event
    // 清理该事件，表示不再关注，也就是说，注册IO事件是一次性的，
    //如果想持续关注某个Socket fd的读写事件，那么每次触发事件后都要重新添加
    //因为不是使用了十六进制位，所以对标志位取反就是相当于将event从events中删除
    // 通过assert(events & event)确保触发的事件(event)的确是之前注册过并感兴趣的，否则说明程序逻辑存在错误，直接终止程序执行。
    // (events & ~event)则表示原来的事件集中移除指定事件
    // 假设events原本为：0110（关注事件2、3）
    // 假设event为：0010（事件2发生）
    // 取反后：~0010 → 1101
    // 进行与运算后：0110 & 1101 → 0100，成功移除了事件2，仅剩事件3
    events = (Event)(events & ~event);

    // trigger
    // 获取当前触发事件(event)所对应的上下文对象(EventContext)
    EventContext& ctx = getEventContext(event);

    //这个过程就相当于scheduler文件中的main.cpp测试一样，把真正要执行的函数放入到任务队列中等线程取出后任务后，协程执行，执行完成后返回主协程继续，执行run方法取任务执行任务(不过可能是不同的线程的协程执行了)。
    // 判断上下文对象存放的是一个回调函数还是一个协程
    if(ctx.cb) {
        // std::cout << "111"<< std::endl;
        // call ScheduleTask(std::function<void()>* f, int thr)
        // 如果是回调函数 (std::function<void()> ctx.cb)，则将该回调封装为调度任务，放入调度器的任务队列。
        ctx.scheduler->scheduleLock(&ctx.cb);
    } else {
        // call ScheduleTask(std::shared_ptr<Fiber>* f, int thr)
        // 如果是协程任务 (std::shared_ptr<Fiber> ctx.fiber)，则直接将协程对象作为任务加入调度队列。
        ctx.scheduler->scheduleLock(&ctx.fiber);
    }   // scheduleLock 是调度器的方法，作用是将任务安全地加入到调度器维护的任务队列中，并唤醒等待取任务的线程进行调度执行

    // reset event context
    // 一旦触发执行完毕，当前事件上下文（ctx）需恢复到初始状态
    resetEventContext(ctx);
    return;
}

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name):
    Scheduler(threads, use_caller, name), TimerManager() {
        // create epoll fd
        // 5000，epoll_create 的参数实际上在现代 Linux 内核中已经被忽略，最早版本的 Linux 中，这个参数用于指定 epoll 内部使用的事件表的大小。
        m_epfd = epoll_create(5000);

        //错误就终止程序
        // 成功返回新创建的epoll实例的文件描述符（正整数）,失败返回-1
        assert(m_epfd > 0);

        // create pipe
        //创建管道的函数规定了m_tickleFds[0]是读端，[1]是写端 
        // 创建一个管道用于唤醒阻塞在epoll_wait的线程
        // m_tickleFds[0]：读端，用于epoll监控。
        // m_tickleFds[1]：写端，当有新任务到来时写入该端，唤醒阻塞的线程
        // pipe() 是 Linux/UNIX 系统提供的一个 系统调用，用于创建一个匿名管道（anonymous pipe），用于进程或线程间通信（IPC）
        // int pipe(int pipefd[2]);
        // 创建一个“单向通信通道”，返回一对文件描述符：
        // 一个用于“读”，一个用于“写”。
        // m_tickleFds[0]：管道的读端（read end）
        // m_tickleFds[1]：管道的写端（write end）
        // 系统会自动创建一个缓冲区，并返回这两个描述符。
        /* 
        在 IOManager 中，pipe() 创建的是一个唤醒 epoll_wait 的手段：
        epoll 会阻塞在 epoll_wait() 等待事件；
        如果这时你想添加任务，需要有方法唤醒 epoll_wait；
        所以：
        把 m_tickleFds[0]（读端）加入 epoll 监听；
        在有任务时向 m_tickleFds[1]（写端）写一个字节；
        读端就会变为“可读”，epoll_wait() 就会被唤醒；
        被唤醒后就可以处理新任务。*/

        /*
        线程 A (epoll_wait)
            |
            |---监听---> m_tickleFds[0] (读端)

        线程 B（或当前线程）
            |
            |---向 m_tickleFds[1] 写数据（写端） → 唤醒 A
        */
        int rt = pipe(m_tickleFds);

        // 成功返回0失败返回-1
        assert(!rt);

        // add read event to epoll
        //将管道的监听注册到epoll上
    //     struct epoll_event {
    //     uint32_t events; /* Epoll events (事件类型) */
    //     epoll_data_t data; /* User data variable (用户数据，一般为文件描述符) */
    // };
        epoll_event event;

        // Edge Triggered，设置标志位，并且采用边缘触发和读事件。
        // EPOLLIN：监听读事件。
        // EPOLLET (Edge Triggered)：边沿触发模式。
        // 边沿触发意味着：仅当pipe从不可读变为可读时触发一次，若不一次性处理完毕，将不会再通知。
        // 仅当文件描述符状态发生变化时触发通知。
        // 状态变化的含义：
        // 从不可读变为可读，或从不可写变为可写的瞬间。
        // 边缘触发模式下，只有在状态变化瞬间（从不可读 → 可读，或从不可写 → 可写）时才通知一次。
        // 如果事件发生后你没有一次性读取完缓冲区中的所有数据，那么即使缓冲区中还有剩余数据，epoll也不会再通知你。
        // 边沿触发就像“门铃”，只在有人按门铃瞬间响一下。如果你开门时没把人接进来，后面再去看门铃，它不会再响。
        // 与水平触发（LT）对比：
        // 水平触发模式（默认模式），只要缓冲区中还有数据可读，就一直通知你（不断响铃），直到读完为止。
        // EPOLLIN：监听读事件（可读即通知）。
        // EPOLLET：事件边沿触发模式（变化瞬间通知一次，不会持续通知）。
        event.events = EPOLLIN | EPOLLET;

        // 指定event.data.fd为pipe的读端，确保epoll监控pipe读端的可读事件。
        // epoll可以监听多个文件描述符（如socket、pipe），这里明确指定了要监听的是哪个文件描述符。
        // epoll监听的是管道的读端，因为只有读端有数据可读时，才需要通知程序处理数据
        event.data.fd = m_tickleFds[0];

        // non-blocked
        //修改管道文件描述符以非阻塞的方式，配合边缘触发。
        // 设置pipe读端为非阻塞模式
        // 防止在读取pipe内容时线程阻塞。
        // 若无数据则直接返回-1，而非等待数据。
        rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);

        //每次需要判断rt是否成功
        // 成功返回0失败返回-1
        assert(!rt);

        //将 m_tickleFds[0];作为读事件放入到event监听集合中
        // 调用epoll_ctl函数，将上面定义好的事件（event）添加到epoll实例（m_epfd）中，开始监控m_tickleFds[0]
        // 成功返回0失败返回-1
        rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
        assert(!rt);

        //初始化了一个包含 32 个文件描述符上下文的数组
        // 事件上下文数组大小初始化
        // 预先分配大小为32个FD的事件上下文对象，减少运行时频繁内存分配的开销。
        contextResize(32);

        //启动 Scheduler，开启线程池，准备处理任务
        // 重点！调度器启动
        start();
        // Scheduler::start() 启动线程池
        // for (size_t i = 0; i < m_threadCount; ++i) {
        //     m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), ...));
        // }
        // 启动多个线程
        // 每个线程执行 Scheduler::run(this)
        // 进入 Scheduler::run() 执行调度主循环
        // std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));
        // 由于 idle() 是虚函数，而 this 是 IOManager*，这里的 this 是 IOManager*，即你传入的是整个派生类对象的指针, 虽然调用的位置在Scheduler::run()里，写的是idle()，但在运行时会检查调用这个函数的真实对象类型。
        // 此时真实对象类型是IOManager，所以会调用IOManager::idle(), 所以这里实际调用的是
        // IOManager::idle() // epoll_wait 事件监听核心逻辑
        /* 
        main()
        └── IOManager::IOManager()
            └── Scheduler::start()
                └── 创建线程 N 个
                        └── 执行 Scheduler::run()
                            └── 构造 idle_fiber → Fiber(std::bind(&Scheduler::idle, this))
                                └── 实际为 IOManager::idle()
                                    └── epoll_wait → 事件驱动调度
        */

    }

IOManager::~IOManager() {
    // 关闭scheduler类中的线程池，让任务全部执行完后线程安全退出
    stop();

    // 关闭epoll的句柄
    // 关闭epoll句柄后，操作系统会自动清理epoll实例相关资源，停止监听事件
    close(m_epfd);

    //关闭管道读端写端
    close(m_tickleFds[0]);
    close(m_tickleFds[1]);

    //将fdcontext文件描述符一个个关闭
    for(size_t i = 0; i < m_fdContexts.size(); ++i) {
        if(m_fdContexts[i]) {
            delete m_fdContexts[i];
        }
    }
}

// no lock
// 函数的用途是调整类内维护的FdContext数组（或容器）大小到指定的size。
void IOManager::contextResize(size_t size) {
    if (size < m_fdContexts.size()) {
        // std::cout <<"1"<< std::endl;
        for (size_t i = size; i < m_fdContexts.size(); ++i) {
            delete m_fdContexts[i]; // 释放多余的FdContext
            m_fdContexts[i] = nullptr;
        }
    }

    //调整m_fdContexts的大小
    // 遍历 m_fdContexts 向量，初始化尚未初始化的 FdContext 对象
    m_fdContexts.resize(size);

    for(size_t i = 0; i < m_fdContexts.size(); ++i) {
        if(m_fdContexts[i] == nullptr) {
            m_fdContexts[i] = new FdContext();

            // 将文件描述符的编号赋值给 fd
            // 设置FdContext的成员fd为当前的索引i
            m_fdContexts[i]->fd = i;
            // std::cout <<i << std::endl;
        }
    }
}

// addEvent方法用于向IO管理器中注册一个事件（如读或写事件）
// fd：文件描述符（通常是socket）。
// event：事件类型（如读事件或写事件）。
// cb：当事件触发时执行的回调函数。
int IOManager::addEvent(int fd, Event event, std::function<void()> cb) {
    // attemp to find FdContext 
    FdContext* fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    // 如果容器m_fdContexts足够大，快速获取对应fd的上下文对象
    if((int)m_fdContexts.size() > fd) {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    } else {
        // 如果大小不足，释放读锁，申请独占写锁，调用contextResize(fd * 1.5)扩展容器，保证容器能够容纳新的fd。
        read_lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        contextResize(fd * 1.5);
        fd_ctx = m_fdContexts[fd];
    }

    // 锁定fd_ctx并检查是否已有事件
    // fd_ctx->mutex是保护FdContext自身状态的互斥锁，确保多个线程不会同时修改。
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // the event has already been added
    // 检查fd_ctx->events是否已经包含当前的事件：
    // 如果已经包含，则表示重复添加事件，直接返回-1，表示失败。
    // 如果不包含，继续往下执行。
    if(fd_ctx->events & event) {
        return -1;
    }

    // add new event
    // 构建epoll事件并调用epoll_ctl
    // 确定调用epoll_ctl的操作类型：
    // 若fd_ctx已有事件，则使用EPOLL_CTL_MOD（修改）。
    // 若还没有注册任何事件，则使用EPOLL_CTL_ADD（新增）。
    int op = fd_ctx->events ? EPOLL_CTL_MOD: EPOLL_CTL_ADD;
    epoll_event epevent;
    epevent.events = EPOLLET | fd_ctx->events | event;
    epevent.data.ptr = fd_ctx;

    // 成功返回0失败返回-1
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);

    if(rt) {
        std::cerr << "addEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    ++m_pendingEventCount;

    // update fdcontext
    // 更新fd_ctx内部的事件状态和回调
    fd_ctx->events = (Event)(fd_ctx->events | event);

    // update event context
    // getEventContext(event) 会返回对应事件的引用：
    // 比如事件为READ或WRITE，分别返回对应的EventContext结构
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);

    // 确保事件上下文未被占用（防御性编程）
    assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);

    // 当前的事件被注册到特定的调度器（当前线程绑定的调度器）
    event_ctx.scheduler = Scheduler::GetThis();

    // 绑定回调函数（回调模式）或绑定协程（协程模式）
    if(cb) {
        // 如果传入的参数 cb 不为空，说明采用回调模式
        // 此时回调函数func2()就已经被存入了event_ctx.cb，等待事件触发时调用
        event_ctx.cb.swap(cb);
    } else {
        // 如果未提供回调函数，默认使用协程模式：
        event_ctx.fiber = Fiber::GetThis();
        // 断言协程状态为 Fiber::RUNNING，说明当前一定处于协程运行状态下调用此函数。
        assert(event_ctx.fiber->getState() == Fiber::RUNNING);
    }
    return 0;
}

// delEvent 函数的作用是从事件管理器中移除一个指定文件描述符（fd）的特定事件（Event）。通常用于取消对某个文件描述符的读写或异常事件监听。
bool IOManager::delEvent(int fd, Event event) {
    // attemp to find FdContext 
    //这里的步骤和上面的addevent添加事件类似
    FdContext* fd_ctx = nullptr;

    // 这里使用读锁（共享锁）安全访问m_fdContexts容器（通常是数组或vector），检查指定的fd是否存在对应的上下文FdContext。
    // 若找到，则保存到fd_ctx指针中；否则直接返回false（表示无法删除）
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fdContexts.size() > fd) {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    } else {
        read_lock.unlock();
        //如果没查找到代表数组中没这个文件描述符直接，返回false；
        return false;
    }

    //找到后添加互斥锁
    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // the event doesn't exist
    // 检查待删除事件是否存在
    if(!(fd_ctx->events & event)) {
        return false;
    }

    // delete the event
    //因为这里要删除事件，对原有的事件状态取反就是删除原有的状态比如说传入参数是读事件，我们取反就是删除了这个读事件但可能还要写事件
    // 使用位运算从原本的事件掩码中移除指定的event
    Event new_events = (Event)(fd_ctx->events & ~event);

    // 如果删除后事件还存在其他监听事件，则修改（EPOLL_CTL_MOD）监听的事件集；
    // 否则，没有任何事件监听了，则从epoll监听集中删除（EPOLL_CTL_DEL）此fd。
    int op = new_events ? EPOLL_CTL_MOD: EPOLL_CTL_DEL;

    epoll_event epevent;
    epevent.events = EPOLLET | new_events;

    //这一步是为了在 epoll 事件触发时能够快速找到与该事件相关联的 FdContext 对象。
    // 并将fd_ctx保存至data.ptr，便于epoll_wait时获取上下文
    epevent.data.ptr = fd_ctx;

    // 调用epoll_ctl更新epoll事件监听状态
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);

    if(rt) {
        std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    //减少了待处理的事件
    --m_pendingEventCount;

    // update fdcontext
    // 更新FdContext的事件掩码
    fd_ctx->events = new_events;

    // update event context
    // 重置事件上下文EventContext
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);

    // 将原本存储在FdContext内的事件上下文（通常包含回调、协程信息等）清理重置，防止内存泄漏或误操作。
    fd_ctx->resetEventContext(event_ctx);
    return true;
}

// 取消并立即触发指定文件描述符（fd）上的特定事件。与delEvent不同的是，它会在取消事件的同时，主动调用与该事件关联的回调函数或协程，使事件处理逻辑立即执行，而不是等待事件实际触发。
bool IOManager::cancelEvent(int fd, Event event) {
    // attemp to find FdContext 
    FdContext* fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fdContexts.size() > fd) {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    } else {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // the event doesn't exist
    if(!(fd_ctx->events & event)) {
        return false;
    }

    // delete the event
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD: EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);

    if(rt) {
        std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return false;
    }
    
    --m_pendingEventCount;

    // update fdcontext, event context and trigger
    //这个代码和上面那个delEvent一致好像就是最后的处理不同一个是重置，一个是调用事件的回调函数
    // 立即触发事件回调
    fd_ctx->triggerEvent(event);
    return true;
}

// 用于取消指定文件描述符（fd）上所有已注册的事件监听，并且立即主动触发所有已注册事件对应的回调函数或协程逻辑。
bool IOManager::cancelAll(int fd) {
    // attemp to find FdContext 
    FdContext* fd_ctx = nullptr;

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if((int)m_fdContexts.size() > fd) {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    } else {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    // none of events exist
    if(!fd_ctx->events) {
        return false;
    }

    // delete all events
    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = 0;
    epevent.data.ptr = fd_ctx;

    // 所有事件清空
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if(rt) {
        std::cerr << "IOManager::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return false;
    }

    // update fdcontext, event context and trigger
    // 逐个检查并触发已注册事件的回调
    // 检测并主动触发所有已注册事件（如读事件、写事件）的回调函数或协程任务。
    // 每触发一个事件的回调，都需要减少全局待处理事件计数器（m_pendingEventCount）。
    if(fd_ctx->events & READ) {
        fd_ctx->triggerEvent(READ);
        --m_pendingEventCount;
    }

    if(fd_ctx->events & WRITE) {
        fd_ctx->triggerEvent(WRITE);
        --m_pendingEventCount;
    }

    // 确认最终fd_ctx->events掩码中事件已被全部清除。
    assert(fd_ctx->events == 0);
    return true;
}

// 用于唤醒当前IO管理器中处于空闲等待（idle）状态的线程。
void IOManager::tickle() {
    // no idle threads
    //这个函数在scheduler检查当前是否有线程处于空闲状态。如果没有空闲线程，函数直接返回，不执行后续操作。
    // 若当前没有任何线程处于空闲（等待事件）的状态，则无需唤醒。
    // 提前返回，避免无意义的唤醒调用。
    if(!hasIdleThreads()) {
        return;
    }

    //如果有空闲线程，函数会向管道 m_tickleFds[1] 写入一个字符 "T"。这个写操作的目的是向等待在 m_tickleFds[0]（管道的另一端）的线程发送一个信号，通知它有新任务可以处理了。
    // 用于唤醒处于阻塞等待状态（例如调用epoll_wait()）的线程
    // ssize_t write(int fd, const void *buf, size_t count);
    // 将缓冲区buf中最多count字节的数据写入到文件描述符fd中。
    // 成功时返回实际写入的字节数（应与count相同），失败则返回-1。
    int rt = write(m_tickleFds[1], "T", 1);
    assert(rt == 1);
}

bool IOManager::stopping() {
    uint64_t timeout = getNextTimer();
    // std::cout << std::boolalpha << (timeout == ~0ull) << std::endl;
    // std::cout << std::boolalpha << (m_pendingEventCount == 0)<< std::endl;
    // std::cout << std::boolalpha<< (Scheduler::stopping() == true) << std::endl;

    // no timers left and no pending events left with the Scheduler::stopping()
    return timeout == ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
}

// 本质是一个运行于Fiber（协程）或独立线程上的事件循环函数，负责监视并处理IO事件与定时任务。
// 该函数利用了Linux的高效I/O复用机制（epoll），并结合超时机制与协程调度，构建一个异步高效的事件驱动模型。
void IOManager::idle() {
    //定义了 epoll_wait 能同时处理的最大事件数。
    static const uint64_t MAX_EVENTS = 256;

    // <epoll_event[]>：
    // 表示管理的是一个动态数组，而不是单个对象。
    // 因此，unique_ptr会调用delete[]释放数组内存
    // 使用 std::unique_ptr 动态分配了一个大小为 MAX_EVENTS 的 epoll_event 数组，用于存储从 epoll_wait 获取的事件
    std::unique_ptr<epoll_event[]> events(new epoll_event[MAX_EVENTS]);

    while(true) {
        if(debug) {
            std::cout << "IOManager::idle(),run in thread: " << Thread::GetThreadId() << std::endl; 
        }

        // 如果IOManager准备停止（stopping()返回true），则退出循环并结束idle()运行
        // 返回false
        if(stopping()) {
            if(debug) {
                std::cout << "name = " << getName() << " idle exits in thread: " << Thread::GetThreadId() << std::endl;
            }
            break;
        }

        // blocked at epoll_wait
        // int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
        // 监控一个或多个文件描述符(fd)上的事件。
        // 如果指定的文件描述符发生了事件（如读、写或错误），它会返回并填充events数组。
        // 若无事件发生，则在timeout毫秒后返回。
        // 如果timeout设为-1，会无限期阻塞直到有事件发生。
        /* 
        epfd：
        epoll 实例的文件描述符，由 epoll_create 或 epoll_create1 创建。
        此处为变量 m_epfd。
        events：
        指向用户分配的数组，内存用于存储返回的事件。
        此处为智能指针 events.get()，指向 epoll_event 类型数组。
        maxevents：
        允许返回的最大事件数量，表示数组最大长度。
        此处为 MAX_EVENTS (比如256)。
        timeout：
        等待超时时间，以毫秒为单位。
        如果 timeout = -1，表示一直阻塞直到有事件发生。
        如果 timeout = 0，表示立即返回（非阻塞）。
        如果 timeout > 0，表示最多阻塞这么多毫秒。*/
        // 返回值含义：
        // > 0 表示触发事件的数量。
        // = 0 表示超时，未有事件发生。
        // < 0 出错，需检查errno确定错误原因。

        // 存放epoll_wait调用的返回值（触发事件的数量或错误码）
        int rt = 0;

        // 无限循环直至epoll_wait成功返回或发生非信号中断错误
        while(true) {
            // 将next_timeout限制在最大5000ms（5秒）内
            static const uint64_t MAX_TIMEOUT = 5000;

            //获取下一个超时的定时器
            uint64_t next_timeout = getNextTimer();
            // std::cout << std::boolalpha<< (~0ull == next_timeout)<< std::endl;   true

            //获取下一个定时器的超时时间，并将其与 MAX_TIMEOUT 取较小值，避免等待时间过长。
            next_timeout = std::min(next_timeout, MAX_TIMEOUT);

            // std::unique_ptr通过get()方法返回其管理的原始指针（裸指针）
            // 注意：此处必须提供C风格裸指针。
            // C++智能指针无法直接隐式转换为原始指针，因此必须显式调用get()
            //epoll_wait陷入阻塞，等待tickle信号的唤醒，
            //并且使用了定时器堆中最早超时的定时器作为epoll_wait超时时间。
            rt = epoll_wait(m_epfd, events.get(), MAX_EVENTS, (int)next_timeout);

            // EINTR -> retry
            // EINTR全称为 "Interrupted system call"（被中断的系统调用），是在Linux/Unix环境中非常常见的一种错误返回值。
            // 当程序调用某些系统函数（例如epoll_wait()、select()、read()等）时，若调用过程中收到了信号（例如SIGALRM、SIGINT、SIGTERM等），系统调用可能会被中断，并立即返回-1，同时设置errno = EINTR。
            // 注意：EINTR并非真正的错误，而是一种中断，提示程序“当前调用未完成，需要重新尝试”。
            if(rt < 0 && errno == EINTR) {
                continue;
            } else {
                // std::cout << "before maybe" << std::endl;
                // std::cout << "maybe" << rt << std::endl;  2
                break;
            }
        }

        // collect all timers overdue
        // 处理到期的定时任务
        //用于存储超时的回调函数。
        std::vector<std::function<void()>> cbs;

        //用来获取所有超时的定时器回调，并将它们添加到 cbs 向量中
        listExpiredCb(cbs);
        if(!cbs.empty()) {
            for(const auto& cb: cbs) {
                // 将定时器回调调度到协程/任务队列中异步执行。
                scheduleLock(cb);
            }
            cbs.clear();
        }

        // collect all events ready
        // 处理epoll_wait返回的所有I/O事件
        // 循环处理此次调用epoll_wait返回的rt个事件
        for(int i = 0; i < rt; ++i) {
            // 获取第 i 个 epoll_event，用于处理该事件。
            epoll_event& event = events[i];
            // std::cout <<std::endl<< i <<std::endl;
            // std::cout << "i: "<< i<< std::boolalpha<< (event.data.fd == m_tickleFds[0])<< std::endl;
            // 0
            // i: 0false

            // 1
            // i: 1true

            // tickle event
            //检查当前事件是否是 tickle 事件（即用于唤醒空闲线程的事件）。
            // 当其他线程添加了新任务或新监听事件时，需要立刻通知（唤醒）阻塞线程重新处理
            // tickle事件（通常是一个管道或eventfd）用于唤醒阻塞在epoll_wait的线程。
            // 采用边缘触发模式（EPOLLET），必须将管道内数据全部读出，否则下次不会再通知。
            if(event.data.fd == m_tickleFds[0]) {
                uint8_t dummy[256];

                // edge triggered -> exhaust
                // epoll_wait侦测到可读事件后返回，如果不及时将管道内的数据读走，下次调用epoll_wait还会持续返回（因为管道内还有数据），造成无效唤醒（busy loop）。
                // 因此必须在接收到事件通知后一次性将管道内数据全部读取完毕。
                // 对于一个管道，如果管道里有多个字节数据（可能是多次写入），epoll_wait只会在管道中有数据可读时触发一次事件。
                // 为了确保每次事件触发都能“消费掉”管道中的所有数据，必须将管道中积累的数据一次性读完。否则，下次epoll_wait被调用时，可能无法再次触发tickle事件。

                // ssize_t read(int fd, void *buf, size_t count);
                // fd：文件描述符（file descriptor），表示要读取的文件、管道、socket 等的标识符。
                // buf：指向缓冲区的指针，存储读取的数据。
                // count：期望读取的字节数（buffer大小）。
                // 返回值（ssize_t，有符号整型）：
                // > 0：成功读取的字节数。
                // = 0：已到达文件末尾（EOF）。
                // < 0：读取失败，出错信息存储在errno中
                while(read(m_tickleFds[0], dummy, sizeof(dummy)) > 0);
                continue;
            }

            // other events
            //通过 event.data.ptr 获取与当前事件关联的 FdContext 指针 fd_ctx，该指针包含了与文件描述符相关的上下文信息。
            // 普通事件处理逻辑：
            // 获取 fd 的上下文，并加锁：
            FdContext* fd_ctx = (FdContext*)event.data.ptr;
            std::lock_guard<std::mutex> lock(fd_ctx->mutex);

            // convert EPOLLERR or EPOLLHUP to -> read or write event
            //如果当前事件是错误或挂起（EPOLLERR 或 EPOLLHUP），则将其转换为可读或可写事件（EPOLLIN 或 EPOLLOUT），以便后续处理。
            // EPOLLERR：表示 fd 上发生了错误（例如 socket 出错）。
            // EPOLLHUP：表示对端关闭连接（例如 socket 被对方关闭）
            if(event.events & (EPOLLERR | EPOLLHUP)) {
                // fd_ctx->events：该 fd 当前用户注册监听的事件集合（READ/WRITE）。
                // EPOLLIN | EPOLLOUT：表示 “可读或可写” 两种事件。
                // 只转化 用户关心的事件，所以要与 fd_ctx->events 做一个 按位与运算
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }

            // events happening during this turn of epoll_wait
            //确定实际发生的事件类型（读取、写入或两者）。
            int real_events = NONE;

            // EPOLLIN 和 EPOLLOUT 是 Linux epoll 事件机制中的事件标志，用于表示你想要“监听什么类型的事件”，或者“某个文件描述符上发生了什么事件”。
            // EPOLLIN 表示：这个 fd 上现在“可读”，也就是：
            // 有数据可读（对于 socket/管道）
            // 文件可读
            // 客户端连接已就绪（对于监听 socket）
            // EPOLLOUT 表示：这个 fd 上现在“可写”，也就是：
            // 发送缓冲区有足够空间可以写入数据
            // 判断 epoll 是否返回了读事件
            if(event.events & EPOLLIN) {
                // 如果 epoll_wait() 返回的 event 包含 EPOLLIN（可读），说明这个 fd 当前可以读了。
                // 把 real_events 标记上 READ（框架内部自定义的枚举）。
                real_events |= READ;
            }

            // 判断 epoll 是否返回了写事件
            if(event.events & EPOLLOUT) {
                real_events |= WRITE;
            }

            // 检查这次返回的事件是否是我们注册监听的
            // 如果 real_events 中的事件我们并没有监听过，就跳过，不处理（例如系统返回了写事件，但我们没监听写）。
            if((fd_ctx->events & real_events) == NONE) {
                continue;
            }

            // delete the events that have already happened
            // 删除已经触发的事件，更新 epoll 监听状态
            // 例如我们监听了 READ + WRITE，但现在只触发了 READ，那么 left_events = WRITE。
            //这里进行取反就是计算剩余未发送的的事件
            int left_events = (fd_ctx->events & ~real_events);

            // std::cout << "left_events: "<< left_events<< std::endl;

            int op = left_events ? EPOLL_CTL_MOD: EPOLL_CTL_DEL;

            //如果left_event没有事件了那么就只剩下边缘触发了events设置了
            event.events = EPOLLET | left_events;

            // 构造新的事件设置，并提交更新
            //根据之前计算的操作（op），调用 epoll_ctl 更新或删除 epoll 监听，如果失败，打印错误并继续处理下一个事件。
            int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
            if(rt2) {
                std::cerr << "idle::epoll_ctl failed: " << strerror(errno) << std::endl; 
                continue;
            }

            // schedule callback and update fdcontext and event context
            //触发事件，事件的执行
            if(real_events & READ) {
                fd_ctx->triggerEvent(READ);
                --m_pendingEventCount;
            }

            if(real_events & WRITE) {
                fd_ctx->triggerEvent(WRITE);
                --m_pendingEventCount;
            }
        }

        //当前线程的协程主动让出控制权，调度器可以选择执行其他任务或再次进入 idle 状态。
        Fiber::GetThis()->yield();
    }
}

// 当一个定时器被插入到定时器队列的最前面时，通知（唤醒）IOManager 的 epoll 线程，重新评估等待时间。
// onTimerInsertedAtFront() 是一个钩子，用于在插入最早定时器时立即唤醒 epoll，使得定时器精确触发。
void IOManager::onTimerInsertedAtFront() {
    // 唤醒可能被阻塞的 epoll_wait 调用
    tickle();
}

}