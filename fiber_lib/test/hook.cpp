#include "hook.h"
#include "ioscheduler.h"
#include <dlfcn.h>
#include <iostream>
#include <cstdarg>
#include "fd_manager.h"
#include <string.h>

// apply XX to all functions
// 此宏定义了一个函数列表，列表中包括了所有希望实现Hook的系统调用
// HOOK_FUN 本质就是批量调用XX()这个函数宏，每次都传入不同的函数名。
#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt) 

namespace sylar {
// if this thread is using hooked function 
//使用线程局部变量，每个线程都会判断一下是否启用了钩子
//表示当前线程是否启用了钩子功能。初始值为 false，即钩子功能默认关闭。
static thread_local bool t_hook_enable = false;

//返回当前线程的钩子功能是否启用。
bool is_hook_enable() {
    return t_hook_enable;
}

//设置当前线程的钩子功能是否启用
void set_hook_enable(bool flag) {
    t_hook_enable = flag;
}

// 初始化Hook机制，主要用来获取系统原始函数的地址并保存到对应的函数指针中。
void hook_init() {
    //通过一个静态变量来确保 hook_init() 只初始化一次，防止重复初始化。
    static bool is_inited = false;
    // 防止重复初始化
    if(is_inited) {
        return;
    }

    // test
    is_inited = true;

// assignment -> sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep"); -> dlsym -> fetch the original symbols/function
// 批量获取原始函数地址
// dlsym是Linux动态链接库的函数，作用是根据函数名字符串查找动态库中原始函数的地址。
// RTLD_NEXT的含义是查找当前链接库后面的（即系统库或下一个库）原始函数地址。
// 例如：sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep");
// 获取系统原始的sleep函数的真实地址，并赋值给函数指针sleep_f。
// 使用宏定义 HOOK_FUN(XX)，批量完成上述任务，而无需逐个手写。
#define XX(name) name##_f = (name##_fun)dlsym(RTLD_NEXT, #name);
HOOK_FUN(XX)
#undef XX    
}

// static variable initialisation will run before the main function
// 这个机制可以确保：
// 在程序正式执行主函数之前，自动完成Hook系统的初始化。
struct HookIniter {
    //钩子函数
    HookIniter() {
        //初始化hook，让原始调用绑定到宏展开的函数指针中
        hook_init();
    }
};

//定义了一个静态的 HookIniter 实例。由于静态变量的初始化发生在 main() 函数之前，所以 hook_init() 会在程序开始时被调用，从而初始化钩子函数。
static HookIniter s_hook_initer;
} // end namespace sylar

struct timer_info {
    int cancelled = 0;
};

// universal template for read and write function
// OriginFun：原始系统调用函数指针类型（如read_fun, write_fun）。
// Args&&... args：变参模板参数，原样转发给系统调用
/*
fd: 文件描述符
fun: 原始系统调用函数
hook_fun_name: 用于调试输出的函数名，便于定位当前Hook的具体函数（如"read", "write"）
event: IO事件类型（如读事件或写事件）一般定义在IO管理器（如sylar::IOManager::READ, sylar::IOManager::WRITE）
timeout_so: 超时的类型（发送或接收超时）一般指定为SO_RCVTIMEO（接收超时）或SO_SNDTIMEO（发送超时）。
args: 其他所有的参数，转发给fun函数

提供了一个通用的Hook机制，封装所有可能会阻塞的IO调用，将其转换为非阻塞协程版调用，并处理事件循环、超时和重试逻辑。

Args&&... args 表示函数可以接受任意个数的参数，并且能以完美转发（perfect forwarding） 的方式传递给其他函数，既保持参数的原始类型与引用特性（左值/右值不变）。
*/
template<typename OriginFun, typename... Args>
// 表示函数为内部链接，仅在定义它的编译单元（cpp文件）中有效。
static ssize_t do_io(int fd, OriginFun fun, const char* hook_fun_name, uint32_t event, int timeout_so, Args&&... args) {
    if(!sylar::t_hook_enable) {
        // 如果没有开启hook，则直接调用原始函数，结束。
        // 这里所有参数的类型、引用性质、左值右值，都原封不动地传递给系统调用函数
        return fun(fd, std::forward<Args>(args)...);
    }

    // 获取文件描述符上下文 (FdCtx)
    // typedef Singleton<FdManager> FdMgr;
    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);
    if(!ctx) {
        // 如果找不到ctx，则不执行hook，直接调用原始函数
        return fun(fd, std::forward<Args>(args)...);
    }

    // 如果fd已经被关闭
    if(ctx->isClosed()) {
        // 设置错误码为EBADF（坏的文件描述符）
        errno = EBADF;
        return -1;
    }

    // 如果当前fd不是socket类型，或用户自己将fd设置成了非阻塞模式，就不会进行hook处理，直接调用原始的系统调用返回。
    // 目的是只对socket类型的阻塞调用进行封装，避免干扰其他IO操作
    if(!ctx->isSocket() || ctx->getUserNonblock()) {
        return fun(fd, std::forward<Args>(args)...);
    }

    // get the timeout
    // 根据当前fd的上下文(FdCtx)获取超时时间（发送或接收超时）。
    uint64_t timeout = ctx->getTimeout(timeout_so);
    
    // timer condition
    // timer_info结构用于后续记录超时事件状态（是否超时）。
    std::shared_ptr<timer_info> tinfo(new timer_info);
/*
调用原始IO函数
   │
   ├─成功 → 返回
   └─失败（EAGAIN）→ 注册事件 & 定时器，挂起协程
         │
         ├─IO事件发生 → 唤醒 → 重新尝试
         └─超时事件发生 → 唤醒 → 返回超时错误
*/
// 标签 retry 和 IO 调用逻辑
retry:
    // run the function
    // 实际调用系统的IO函数。
    ssize_t n = fun(fd, std::forward<Args>(args)...);
    
    // EINTR ->Operation interrupted by system ->retry
    // 如果调用过程中因为信号中断(EINTR)而失败，则自动重试，确保调用得到明确结果（成功或其他错误）。
    while(n == -1 && errno == EINTR) {
        n = fun(fd, std::forward<Args>(args)...);
    }

    // 0 resource was temporarily unavailable -> retry until ready 
    // 非阻塞IO重试处理逻辑(EAGAIN)
    // 在非阻塞模式下，资源暂时不可用时，IO调用返回EAGAIN。
    // 这里处理这种情况，通过异步事件机制等待资源可用
    if(n == -1 && errno == EAGAIN) {
        sylar::IOManager* iom = sylar::IOManager::GetThis();
        // timer
        std::shared_ptr<sylar::Timer> timer;
        std::weak_ptr<timer_info> winfo(tinfo);

        // 1 timeout has been set -> add a conditional timer for canceling this operation
        // 如果设置了超时时间，注册一个定时器
        if(timeout != (uint64_t)-1) {
/*
参数1 (timeout)：定时器的超时时间（毫秒）。
参数2（lambda）：定时器触发后执行的回调函数。
参数3 (winfo)：条件对象的弱引用（weak_ptr），用于在定时器执行时判断目标对象是否有效，避免对象已经销毁后访问。
*/
            timer = iom->addConditionTimer(timeout, [winfo, fd, iom, event]() {
                // 将弱引用（weak_ptr）提升为强引用（shared_ptr）
                auto t = winfo.lock();

                // 若提升失败（!t），说明原对象(timer_info)已被销毁，直接返回，不执行后续逻辑。
                if(!t || t->cancelled) {
                    return;
                }

                // 置位标志cancelled，表明此次操作已超时。
                // 这里使用标准错误码ETIMEDOUT（连接或操作超时）
                t->cancelled = ETIMEDOUT;

                // cancel this event and trigger once to return to this fiber
                // 调用IOManager的cancelEvent方法，取消之前注册的fd上对应事件。
                iom->cancelEvent(fd, (sylar::IOManager::Event)(event));
            }, winfo);
        }

        // 2 add event -> callback is this fiber
        // 注册当前协程到IOManager，表示当前协程需要等待
        int rt = iom->addEvent(fd, (sylar::IOManager::Event)(event));
        if(rt) {
            // 注册事件失败的处理
            std::cout << hook_fun_name << " addEvent("<< fd << ", " << event << ")";

            // 若定时器已注册，则主动调用timer->cancel()取消定时器，避免不必要的超时回调。
            if(timer) {
                timer->cancel();
            }
            errno = EINVAL;  // 设置明确的错误码
            return -1;
        } else {
            // 协程挂起等待事件发生或超时
            sylar::Fiber::GetThis()->yield();

            // 3 resume either by addEvent or cancelEvent
            // 协程恢复后，无论是否超时，都应主动取消定时器。
            // 防止定时器未触发，但后续已经无需超时控制的情况，避免无效定时器回调
            // 假设你在煮饭，设置了个闹钟（定时器）提醒你30分钟后必须关火（超时）。
            // 如果饭熟了（IO事件发生），你主动关了火。
            // 此时闹钟已经没用了，必须要手动关掉闹钟，不然过了一会闹钟响了，又提醒你去关火一次，是不是很奇怪？
            // 同理，协程恢复后也要“关掉闹钟”，防止后续无用的提醒。
            if(timer) {
                timer->cancel();
            }

            // by cancelEvent
            // 判断是否超时
            // 若为ETIMEDOUT，代表协程被超时机制唤醒。
            // 设置系统错误码errno为ETIMEDOUT。
            // 返回-1表明操作超时。
            if(tinfo->cancelled == ETIMEDOUT) {
                errno = tinfo->cancelled;
                return -1;
            }
            // 重新尝试IO操作 (goto retry)
            goto retry;
        }
    }
    return n;
}

extern "C" {

// 这里利用宏定义和HOOK_FUN机制批量定义hook的函数指针（例如sleep_f），
// sleep_f是原始系统调用函数的指针（如sleep函数原始地址）。
// 作用：允许调用系统原始函数（非hook状态）。
// 定义函数指针，用来保存原始的系统调用
// declaration -> sleep_fun sleep_f = nullptr;
#define XX(name) name##_fun name##_f = nullptr;
HOOK_FUN(XX)
#undef XX

// only use at task fiber
unsigned int sleep(unsigned int seconds) {
    // 如果全局hook功能未开启，则调用原生系统函数sleep_f（阻塞式）。
    // 此时表现与原生sleep一致。
    if(!sylar::t_hook_enable) {
        return sleep_f(seconds);
    }

    // 获取当前执行的协程对象
    std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();

    // 获取当前协程调度器(IOManager)
    sylar::IOManager* iom = sylar::IOManager::GetThis();

    // add a timer to reschedule this fiber
    // seconds * 1000：睡眠时间，单位是毫秒
    // lambda的作用是唤醒协程：
    // scheduleLock用于把之前挂起（睡眠）的协程重新放入执行队列中，准备恢复执行。
    iom->addTimer(seconds * 1000, [fiber, iom]() {
        iom->scheduleLock(fiber, -1);
    });

    // wait for the next resume
    // 协程主动挂起，进入等待状态。
    // 此时线程不会阻塞，线程可以继续执行其他任务或协程。
    // 协程直到定时器到期，才会恢复执行
    fiber->yield();
    return 0;
}

// useconds_t usec表示睡眠的微秒数 (us表示 microseconds)
int usleep(useconds_t usec) {
    if(!sylar::t_hook_enable) {
        return usleep_f(usec);
    }

    //这里的步骤和sleep函数类似。
    std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
    sylar::IOManager* iom = sylar::IOManager::GetThis();

    // add a timer to reschedule this fiber
    iom->addTimer(usec / 1000, [fiber, iom]() {
        iom->scheduleLock(fiber);
    });

    // wait for the next resume
	fiber->yield();
	return 0;
}

// req: 请求的睡眠时长 (timespec结构包含秒tv_sec和纳秒tv_nsec)。
// rem: 当sleep被信号中断时，剩余的未睡眠时长会被写入rem（本实现简化，未考虑中断情况）。
int nanosleep(const struct timespec* req, struct timespec* rem) {
    if(!sylar::t_hook_enable) {
        return nanosleep_f(req, rem);
    }
    
    //timeout_ms 将 tv_sec 转换为毫秒，并将 tv_nsec 转换为毫秒，然后两者相加得到总的超时毫秒数。所以从这里看出实现的也是一个毫秒级的操作。
    // 将用户输入的睡眠时长转换为毫秒（ms）：
    // 秒(tv_sec)转为毫秒: tv_sec * 1000
    // 纳秒(tv_nsec)转为毫秒: tv_nsec / 1000000
    // req = { tv_sec = 1, tv_nsec = 500000000 } 
    // 计算结果 timeout_ms = 1000 + 500 = 1500ms
    int timeout_ms = req->tv_sec * 1000 + req->tv_nsec / 1000 / 1000;

    // 获取当前协程和调度器
    std::shared_ptr<sylar::Fiber> fiber = sylar::Fiber::GetThis();
    sylar::IOManager* iom = sylar::IOManager::GetThis();

    // add a timer to reschedule this fiber
	iom->addTimer(timeout_ms, [fiber, iom](){iom->scheduleLock(fiber, -1);});
	// wait for the next resume
	fiber->yield();	
	return 0;
}

/*
这完全遵循了原生socket函数的定义和接口参数：
domain：套接字所属的协议族（如AF_INET表示IPv4）。
type：套接字类型（如SOCK_STREAM表示TCP，SOCK_DGRAM表示UDP）。
protocol：指定具体协议（通常填0自动推断）。
*/
int socket(int domain, int type, int protocol) {
    if(!sylar::t_hook_enable) {
        return socket_f(domain, type, protocol);
    }

    //如果钩子启用了，则通过调用原始的 socket 函数创建套接字，并将返回的文件描述符存储在 fd 变量中。
    int fd = socket_f(domain, type, protocol);

    //fd是无效的情况
    if(fd == -1) {
        std::cerr << "socket() failed:" << strerror(errno) << std::endl;
		return fd;
    }

    //如果socket创建成功会利用Fdmanager的文件描述符管理类来进行管理，判断是否在其管理的文件描述符中，如果不在扩展存储文件描述数组大小，并且利用FDctx进行初始化判断是是不是套接字，是不是系统非阻塞模式。
    sylar::FdMgr::GetInstance()->get(fd, true);
    return fd;
}

/*
这个函数的主要功能是支持超时控制的非阻塞connect，通过协程机制实现：
原生connect系统调用默认阻塞线程，无法指定超时时间。
通过hook后的connect_with_timeout函数，允许设定自定义超时时间，并通过协程机制挂起协程，而不阻塞线程。

fd: 套接字文件描述符。
addr: 指向要连接的目标地址结构。
addrlen: 地址结构的长度。
timeout_ms: 超时时间（单位为毫秒，ms）。
*/
int connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout_ms) {
    // 检查是否启用了hook功能
    if(!sylar::t_hook_enable) {
        return connect_f(fd, addr, addrlen);
    }

    //获取文件描述符 fd 的上下文信息 FdCtx
    // 通过FdMgr单例获取对应fd的上下文信息（FdCtx）
    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);

    //检查文件描述符上下文是否存在或是否已关闭。
    // 若fd未注册或已关闭，则返回错误，设置errno为EBADF（坏的文件描述符）
    if(!ctx || ctx->isClosed()) {
        //EBAD表示一个无效的文件描述符
        errno = EBADF;
        return -1;
    }

    //如果不是一个套接字调用原始的
    if(!ctx->isSocket()) {
        return connect_f(fd, addr, addrlen);
    }

    //检查用户是否设置了非阻塞模式。如果是非阻塞模式，
    if(ctx->getUserNonblock()) {
        return connect_f(fd, addr, addrlen);
    }

    // attempt to connect
    //尝试进行 connect 操作，返回值存储在 n 中。
    int n = connect_f(fd, addr, addrlen);
    // 若connect直接返回成功，直接返回0，表示连接成功。
    if(n == 0) {
        return 0;
    } else if(n != -1 || errno != EINPROGRESS) {
        //说明连接请求未处于等待状态，直接返回结果。
        return n;
    }
    
    // 若返回-1，且errno==EINPROGRESS，表示连接正在进行中（非阻塞模式特有返回值），需等待socket可写事件。
    // wait for write event is ready -> connect succeeds
    //获取当前线程的 IOManager 实例。
    sylar::IOManager* iom = sylar::IOManager::GetThis();

    //声明一个定时器对象。
    std::shared_ptr<sylar::Timer> timer;

    //创建追踪定时器是否取消的对象
    std::shared_ptr<timer_info> tinfo(new timer_info);

    //判断追踪定时器对象是否存在
    std::weak_ptr<timer_info> winfo(tinfo);

    //检查是否设置了超时时间。如果 timeout_ms 不等于 -1，则创建一个定时器
    if(timeout_ms != (uint64_t)-1) {
        //添加一个定时器，当超时时间到达时，取消事件监听并设置 cancelled 状态。
        timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom]() {
            auto t = winfo.lock();

            //判断追踪定时器对象是否存在或者追踪定时器的成员变量是否大于0.大于0就意味着取消了
            if(!t || t->cancelled) {
                return;
            }

            //如果超时了但时间仍然未处理
            // 定时器回调触发 → 设置cancelled=ETIMEDOUT
            t->cancelled = ETIMEDOUT;

            //将指定的fd的事件触发将事件处理。
            // 取消监听事件并强制触发事件
            // 我们在等待fd变为可写（连接成功时fd自动变可写）
            iom->cancelEvent(fd, sylar::IOManager::WRITE);
        }, winfo);
    }

    //为文件描述符 fd 添加一个写事件监听器。这样的目的是为了上面的回调函数处理指定文件描述符
    // 协程想要等待连接完成，所以必须监听fd变为可写状态
    // 这一步是开启事件监听，告诉IOManager：
    // “我要等待这个fd的WRITE事件（连接成功时fd变可写），事件发生时请唤醒我。”
    int rt = iom->addEvent(fd, sylar::IOManager::WRITE);

    //返回0表示注册监听成功
    if(rt == 0) {
        // 如果addEvent注册成功（返回0），协程立即调用yield()主动挂起：
        // 协程进入休眠状态，线程释放出来去执行其他协程。
        // 此时协程不会占用CPU和线程，达到高效并发的目的。
        sylar::Fiber::GetThis()->yield();

        // resume either by addEvent or cancelEvent
        //如果有定时器，取消定时器。
        if(timer) {
            timer->cancel();
        } 
        
        //发生超时错误或者用户取消
        if(tinfo->cancelled) {
            //赋值给errno通过其查看具体错误原因。
            errno = tinfo->cancelled;
            return -1;
        }
    } else {
        // 若最初的addEvent本身失败（返回非零），事件监听根本未注册成功
        //添加事件失败
        if(timer) {
            timer->cancel();
        }
        std::cerr << "connect addEvent(" << fd << ", WRITE) error";
    }

    // check out if the connection socket established 
    // 检查连接套接字是否真正建立成功（真正确认连接结果）
    int error = 0;

    // 明确告诉编译器这是socket长度参数
    socklen_t len = sizeof(int);

    //通过getsocketopt检查套接字实际错误状态
    //来判断是否成功或失败。
    // 为了明确知道连接是否真的成功，必须调用getsockopt来查询真实连接状态
    /*
    int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
    sockfd：套接字文件描述符。
    level：表示你要获取的选项所在的层次。常用值：SOL_SOCKET：表示套接字层次的通用选项。
                                IPPROTO_TCP：表示TCP层次的特定选项（如TCP_KEEPALIVE）
    optname：具体的选项名称，常见的SOL_SOCKET选项包括：
            SO_ERROR：获取并清除套接字上最近一次的错误状态。
            SO_REUSEADDR：地址复用选项。
            这里使用SO_ERROR表示查询连接的实际错误状态。
    optval：指针，用于保存查询到的错误码（连接状态）。
    optlen：传入时表示optval缓冲区大小，传出时保存实际返回的数据长度。
    返回值为0：函数调用本身成功（查询到了连接状态）。
    返回值为-1：函数调用失败（例如套接字无效），此时应立即返回错误
    */
    if(-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) {
        return -1;
    }

    //如果没有错误，返回 0 表示连接成功。
    if(!error) {
        return 0;
    } else {
        //如果有错误，设置 errno 并返回错误。
        errno = error;
        return -1;
    }
}

// 网络库或框架中：对connect函数做统一的封装，以便管理连接超时时间
//s_connect_timeout 是一个 static 变量，表示默认的连接超时时间，类型为 uint64_t，可以存储 64 位无符号整数。
//-1 通常用于表示一个无效或未设置的值。由于它是无符号整数，-1 实际上会被解释为 UINT64_MAX，表示没有超时限制
// uint64_t为64位无符号整数类型，通常用于表示毫秒或微秒级的超时时间。
// 初始赋值为-1，由于s_connect_timeout是无符号类型，因此实际存储的值为UINT64_MAX，通常代表“不设超时”或“无限超时”。
static uint64_t s_connect_timeout = -1;
int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    //调用hook启用后的connect_with_timeout函数
    return connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
}

int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    int fd = do_io(sockfd, accept_f, "accept", sylar::IOManager::READ, SO_RCVTIMEO, addr, addrlen);

    if(fd >= 0) {
        //添加到文件描述符管理器FdManager中
        sylar::FdMgr::GetInstance()->get(fd, true);
    }

    return fd;
}

ssize_t read(int fd, void* buf, size_t count) {
    return do_io(fd, read_f, "read", sylar::IOManager::READ, SO_RCVTIMEO, buf, count);
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
	return do_io(fd, readv_f, "readv", sylar::IOManager::READ, SO_RCVTIMEO, iov, iovcnt);	
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
	return do_io(sockfd, recv_f, "recv", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags);	
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
	return do_io(sockfd, recvfrom_f, "recvfrom", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);	
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
	return do_io(sockfd, recvmsg_f, "recvmsg", sylar::IOManager::READ, SO_RCVTIMEO, msg, flags);	
}

ssize_t write(int fd, const void *buf, size_t count)
{
	return do_io(fd, write_f, "write", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, count);	
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
	return do_io(fd, writev_f, "writev", sylar::IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);	
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
	return do_io(sockfd, send_f, "send", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags);	
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
{
	return do_io(sockfd, sendto_f, "sendto", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, len, flags, dest_addr, addrlen);	
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
	return do_io(sockfd, sendmsg_f, "sendmsg", sylar::IOManager::WRITE, SO_SNDTIMEO, msg, flags);	
}

int close(int fd) {
    if(!sylar::t_hook_enable) {
        return close_f(fd);
    }

    // 获取文件描述符上下文（FdCtx）
    std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);

    // 检查并取消所有事件
    if(ctx) {
        auto iom = sylar::IOManager::GetThis();
        if(iom) {
            // 通过IOManager取消所有与该文件描述符相关的异步事件。
            iom->cancelAll(fd);
        }

        // del fdctx
        //  删除文件描述符上下文（FdCtx）
        sylar::FdMgr::GetInstance()->del(fd);
    }
    //处理完后调用原始系统调用
    return close_f(fd);
}

int fcntl(int fd, int cmd, ...) {
    // to access a list of mutable parameters
    va_list va;

    // 初始化参数列表，让va指向第一个可变参数
    //使其指向第一个可变参数（在 cmd 之后的参数）
    va_start(va, cmd);

    switch(cmd) {
        // 设置文件状态标志
        //用于设置文件描述符的状态标志（例如，设置非阻塞模式）
        case F_SETFL:
            {
                // Access the next int argument
                // 从参数列表取出一个整型参数arg，即文件状态标志
                int arg = va_arg(va, int);
                va_end(va);

                // 使用FdMgr获取对应fd的上下文对象FdCtx
                std::shared_ptr<sylar::FdCtx> ctx;
                {
                    // 限定在一个局部范围内释放锁
                    ctx = sylar::FdMgr::GetInstance()->get(fd); 
                    // 后续操作不要再调用可能锁定FdManager的函数
                }

                //如果ctx无效，或者文件描述符关闭不是一个套接字就调用原始调用
                if(!ctx || ctx->isClosed() || !ctx->isSocket()) {
                    return fcntl_f(fd, cmd, arg);
                }

                // 用户是否设定了非阻塞
                // arg & O_NONBLOCK用于提取用户是否设置了非阻塞模式：
                // 若设置非阻塞，则存储true；
                // 若未设置，则存储false。
                ctx->setUserNonblock(arg & O_NONBLOCK);

                // 最后是否阻塞根据系统设置决定
                if(ctx->getSysNonblock()) {
                    arg |= O_NONBLOCK;
                } else {
                    // 实际设置为阻塞
                    arg &= ~O_NONBLOCK;
                }

                return fcntl_f(fd, cmd, arg);
            }
            break;
        // 获取文件状态标志
        case F_GETFL:
            {
                va_end(va);

                //调用原始的 fcntl 函数获取文件描述符的当前状态标志。
                int arg = fcntl_f(fd, cmd);

                std::shared_ptr<sylar::FdCtx> ctx;
                {
                    // 尽量避免全局锁持有
                    ctx = sylar::FdMgr::GetInstance()->get(fd);
                }

                //如果上下文无效、文件描述符已关闭或不是套接字，则直接返回状态标志
                if(!ctx || ctx->isClosed() || !ctx->isSocket()) {
                    return arg;
                }

                // 这里是呈现给用户 显示的为用户设定的值
                // 但是底层还是根据系统设置决定的
                if(ctx->getUserNonblock()) {
                    return arg | O_NONBLOCK;
                } else {
                    return arg & ~O_NONBLOCK;
                }
            }
            break;
        // 这些命令（F_DUPFD、F_DUPFD_CLOEXEC、F_SETFD 等）都会共享执行同一段代码
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
// F_SETPIPE_SZ 用于设置管道（pipe）的缓冲区大小。
// 此命令是Linux内核特定扩展，不同的平台可能不支持，所以用条件编译#ifdef进行判断。
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
            {
                //从va获取标志位
                int arg = va_arg(va, int);

                //清理va
                va_end(va);

                //调用原始调用
                return fcntl_f(fd, cmd, arg); 
            }
            break;

        // 不需要额外参数的命令处理
        // 获取文件描述符的标志（如close-on-exec）
        case F_GETFD:
        // 获取当前文件描述符关联的进程或进程组ID
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
            {
                va_end(va);
                return fcntl_f(fd, cmd);
            }
            break;

        //设置文件锁，如果不能立即获得锁，则返回失败。
        // 非阻塞地尝试给文件加锁，若文件已被锁定则立即返回错误
        // 文件锁允许进程独占或共享访问文件特定区域，防止多个进程同时读写文件导致数据混乱。
        case F_SETLK:

        //设置文件锁，且如果不能立即获得锁，则阻塞等待。
        // 阻塞地尝试给文件加锁，若文件已被锁定则等待直到锁可用
        case F_SETLKW:

        // 获取当前文件的锁状态，返回锁的信息，但不实际锁定文件
        //获取文件锁的状态。如果文件描述符 fd 关联的文件已经被锁定，那么该命令会填充 flock 结构体，指示锁的状态。
        case F_GETLK:
            {
                //从可变参数列表中获取 struct flock* 类型的指针，这个指针指向一个 flock 结构体，包含锁定操作相关的信息（如锁的类型、偏移量、锁的长度等）
                struct flock* arg = va_arg(va, struct flock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;

        /*
            struct f_owner_exlock {
            int type;  // 指定所有者类型
                    // F_OWNER_TID  (线程)
                    // F_OWNER_PID  (进程)
                    // F_OWNER_PGRP (进程组)

            pid_t pid; // 对应的进程ID或线程ID
        };
        */
        // 一般用于异步 I/O 或信号驱动 I/O 中指定谁会接收特定的 I/O 信号或通知
        //获取文件描述符 fd 所属的所有者信息。这通常用于与信号处理相关的操作，尤其是在异步 I/O 操作中。
        case F_GETOWN_EX:

        //设置文件描述符 fd 的所有者信息
        case F_SETOWN_EX:
            {
                //从可变参数中提取相应类型的结构体指针
                struct f_owner_exlock* arg = va_arg(va, struct f_owner_exlock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;

        default:
            va_end(va);
            return fcntl_f(fd, cmd);
    }
}

int ioctl(int fd, unsigned long request, ...) {
    //va持有处理可变参数的状态信息
    // 定义一个变量va用于保存可变参数的处理状态
    va_list va;

    //给va初始化让它指向可变参数的第一个参数位置。
    // 初始化va变量，指向可变参数列表中的第一个参数。
    // 第二个参数request为最后一个已知参数
    va_start(va, request);

    //将va的指向参数的以void*类型取出存放到arg中
    // 从参数列表中取出一个参数，存储在arg中。
    // 这里假设传入的第一个可变参数是一个指针类型（一般 ioctl 接口都是如此）
    void* arg = va_arg(va, void*);

    //用于结束对 va_list 变量的操作。清理va占用的资源
    // 表示可变参数处理结束，释放资源，避免资源泄漏
    va_end(va);

    //用于设置非阻塞模式的命令
    // 检测当前的请求（request）是否为FIONBIO。
    // FIONBIO是标准命令，用于在套接字或文件描述符上设置或清除非阻塞模式
    if(FIONBIO == request) {
        //当前 ioctl 调用是为了设置或清除非阻塞模式。
        // 先将arg（指针）转换为整型指针 (int*)，然后取出值*(int*)arg。
        // 再用双重逻辑非!!操作符，将其转换为明确的布尔值：
        // 若原值为0，则user_nonblock为false（阻塞模式）。
        // 若原值非0，则user_nonblock为true（非阻塞模式）
        bool user_nonblock = !!*(int*)arg;

        // 通过sylar::FdMgr（文件描述符管理器）获取对应于文件描述符fd的上下文对象（FdCtx）
        std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(fd);

        //检查获取的上下文对象是否有效（即 ctx 是否为空）。如果上下文对象无效、文件描述符已关闭或不是一个套接字，则直接调用原始的 ioctl 函数，返回处理结果。
        if(!ctx || ctx->isClosed() || !ctx->isSocket()) {
            return ioctl_f(fd, request, arg);
        }

        //如果上下文对象有效，调用其 setUserNonblock 方法，将非阻塞模式设置为 user_nonblock 指定的值。这将更新文件描述符的非阻塞状态。
        // 更新上下文中的非阻塞模式状态
        ctx->setUserNonblock(user_nonblock);
    }
    return ioctl_f(fd, request, arg);
}

int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen) {
    return getsockopt_f(sockfd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
    if(!sylar::t_hook_enable) {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }

    //如果 level 是 SOL_SOCKET 且 optname 是 SO_RCVTIMEO（接收超时）或 SO_SNDTIMEO（发送超时），代码会获取与该文件描述符关联的 FdCtx 上下文对象：
    // 判断是否为套接字级别（SOL_SOCKET）的超时选项
    if(level == SOL_SOCKET) {
        if(optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
            // 获取文件描述符的上下文对象（FdCtx）
            std::shared_ptr<sylar::FdCtx> ctx = sylar::FdMgr::GetInstance()->get(sockfd);

            //那么代码会读取传入的 timeval 结构体，将其转化为毫秒数，并调用 ctx->setTimeout 方法，记录超时设置：
            // 如果上下文对象有效，则记录超时时间到上下文
            /*
            struct timeval {
                time_t tv_sec;   // 秒数
                suseconds_t tv_usec; // 微秒数
            };
            */
            if(ctx) {
                const timeval* v = (const timeval*)optval;
                // 毫秒数 = 秒数 * 1000 + 微秒数 / 1000;
                ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }
    //无论是否执行了超时处理，最后都会调用原始的 setsockopt_f 函数来设置实际的套接字选项。
    return setsockopt_f(sockfd, level, optname, optval, optlen);	
}

}