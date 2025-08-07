#ifndef __SYLAR_IOMANAGER_H__
#define __SYLAR_IOMANAGER_H__

#include "scheduler.h"
#include "timer.h"

namespace sylar {
// work flow
// 1 register one event -> 2 wait for it to ready -> 3 schedule the callback -> 4 unregister the event -> 5 run the callback
// 1 注册事件 -> 2 等待事件 -> 3 事件触发调度回调 -> 4 注销事件回调后从epoll注销 -> 5 执行回调进入调度器中执行调度。

// 主要用于管理异步 IO 事件，并结合调度器和定时器功能进行高效的事件处理。该类基于事件驱动模型设计，适用于高性能的网络或文件描述符管理
// IOManager 类继承自 Scheduler 和 TimerManager，因此它具有调度任务和管理定时器的能力。
class IOManager: public Scheduler, public TimerManager {
public:
    // 内部枚举
    // Event 是一个枚举类型，表示文件描述符上的事件类型
    enum Event {
        // 表示没有事件
        NONE = 0x0,

        // 表示读事件，对应于 epoll 的 EPOLLIN 事件
        // READ == EPOLLIN
        READ = 0x1,

        // 表示写事件，对应于 epoll 的 EPOLLOUT 事件
        // WRITE == EPOLLOUT
        WRITE = 0x4
    };

private:
    // 用于描述一个文件描述的事件上下文
    // FdContext 结构体用于存储每个文件描述符的事件上下文。每个文件描述符可以有两个事件上下文：read 和 write，分别对应读事件和写事件
    struct FdContext {
        // 描述一个具体事件的上下文，如读事件或写事件 
        struct EventContext {
            // scheduler
            // 关联的调度器 用于安排回调的执行 
            Scheduler* scheduler = nullptr;

            // callback fiber
            // 协程对象，表示回调函数运行时的上下文。
            std::shared_ptr<Fiber> fiber;

            // callback function
            // 关联的回调函数 事件触发时会执行该函数。
            std::function<void()> cb;
        };

        // read event context
        // read 和write表示读和写的上下文
        EventContext read;

        // write event context
        EventContext write;
        int fd = 0;

        // events registered
        // 当前注册的事件，表示当前文件描述符上注册的事件类型。它的值可以是 NONE、READ、WRITE 或者 READ | WRITE（组合事件）。这个变量用于标识哪些事件正在被监视和处理。
        Event events = NONE;

        // 用于保护 FdContext 数据的互斥锁。由于 IOManager 可能在多线程环境中运行，mutex 保证了在并发环境中对文件描述符上下文的安全访问，避免竞态条件
        std::mutex mutex;

        // 根据事件类型获取相应的事件上下文（如读事件上下文或写事件上下文）
        // 根据事件类型获取相应的事件上下文（read 或 write）。这个方法根据传入的 event（如 READ 或 WRITE）返回对应的 EventContext
        EventContext& getEventContext(Event event);

        // 重置事件上下文
        // 重置事件上下文的状态。这个方法会将事件上下文中的成员变量（如 scheduler、fiber 和 cb）重置为初始状态，通常在事件完成处理后调用，以准备好下一次事件的注册。
        void resetEventContext(EventContext& ctx);

        // 触发事件
        // 触发事件，执行与事件相关的回调。调用此方法会根据事件类型（如 READ 或 WRITE）执行相应的回调函数。
        void triggerEvent(Event event);
    };
public:
    // 允许设置线程数量、是否使用调用者线程以及名称。
    // threads线程数量，use_caller是否将主线程或调度线程包含进去，name调度器的名字
    // 定是否使用调用者线程来执行事件处理。默认值为 true，表示在 IOManager 中，调用者线程也会被用于执行 I/O 操作
    IOManager(size_t threads = 1, bool use_caller = true, const std::string& name = "IOManager");
    ~IOManager();

    // add one event at a time
    // 事件管理方法
    // 添加一个事件到文件描述符 fd 上，并关联一个回调函数 cb。
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);

    // delete event
    // 删除文件描述符fd上的某个事件
    // 删除某个文件描述符上的特定事件。取消该事件的监视。
    bool delEvent(int fd, Event event);

    // delete the event and trigger its callback
    // 取消文件描述符上的某个事件，并触发其回调函数
    bool cancelEvent(int fd, Event event);

    // delete all events and trigger its callback
    // 取消文件描述符 fd 上的所有事件，并触发所有回调函数。
    bool cancelAll(int fd);

    // 获取当前的 IOManager 实例
    static IOManager* GetThis();

protected:
    //通知调度器有任务调度
    //写pipe让idle协程从epoll_wait退出，待idle协程yield之后Scheduler::run就可以调度其他任务.
    void tickle() override;

    //判断调度器是否可以停止
    //判断条件是Scheduler::stopping()外加IOManager的m_pendingEventCount为0，表示没有IO事件可调度
    bool stopping() override;

    //实际是idle协程只负责收集所有已触发的fd的回调函数并将其加⼊调度器
    //的任务队列，真正的执⾏时机是idle协程退出后，调度器在下⼀轮调度时执⾏
    //这里也是scheduler的重写，当没有事件处理时，线程处于空闲状态时的处理逻辑。
    void idle() override;

    //因为Timer类的成员函数重写当有新的定时器插入到前面时的处理逻辑
    void onTimerInsertedAtFront() override;

    // 调整 IOManager 内部上下文（例如，文件描述符上下文）的大小。这可能用于动态增加或减少事件处理的容量
    void contextResize(size_t size);

private:
    //用于epoll的文件描述符。
    // fd[0] read，fd[1] write
    int m_epfd = 0;

    //用于线程间通信的管道文件描述符，fd[0] 是读端，fd[1] 是写端。
    int m_tickleFds[2];

    //原子计数器，用于记录待处理的事件数量。使用atomic的好处是这个变量再进行加或-都是不会被多线程影响
    // 原子变量，表示当前挂起的事件数量。使用 atomic 类型确保在多线程环境下的并发访问不会出现问题。
    std::atomic<size_t> m_pendingEventCount = {0};

    // 读写锁
    std::shared_mutex m_mutex;

    // store fdcontexts for each fd
    //文件描述符上下文数组，用于存储每个文件描述符的 FdContext。
    std::vector<FdContext*> m_fdContexts;
};
}
#endif