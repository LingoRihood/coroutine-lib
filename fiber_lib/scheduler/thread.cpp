#include "thread.h"

#include <sys/syscall.h>
#include <iostream>
#include <unistd.h>

namespace sylar {
// 线程信息
// static 使得变量 t_thread 的生命周期贯穿整个程序运行期间，但是它的作用域仅限于当前文件（或当前翻译单元），意味着它不会被外部链接
// 这定义了一个指向 Thread 类的指针 t_thread，它是每个线程的局部变量，并且初始化为 nullptr。
static thread_local Thread* t_thread = nullptr;
static thread_local std::string t_thread_name = "UNKNOWN";

// pid_t 是一个用于表示进程ID（Process ID）或线程ID（Thread ID）的类型。通常在 Linux 系统中，它是一个整数类型（int），用于标识进程或线程。
pid_t Thread::GetThreadId() {
    // syscall 是一个系统调用接口，可以让你直接调用操作系统提供的底层功能。
    // SYS_gettid 是 Linux 系统调用号，表示获取当前线程的线程ID（gettid）。
    // syscall(SYS_gettid) 实际上是执行 gettid() 系统调用的操作，返回当前线程的线程ID。
    // 该调用返回当前线程的线程ID，通常与 pthread_self() 的返回值相同，但是 gettid 是返回内核级线程ID，而 pthread_self() 返回的是 POSIX 线程库级别的线程ID
    // SYS_gettid 是一个常量，表示获取当前线程ID的系统调用号。
    // 每个系统调用都有一个唯一的编号（常量），用于标识该系统调用。SYS_gettid 对应的是获取线程ID的操作。
    return syscall(SYS_gettid);
}

Thread* Thread::GetThis() {
    return t_thread;
}

const std::string& Thread::GetName() {
    return t_thread_name;
}

void Thread::SetName(const std::string& name) {
    if(t_thread) {
        t_thread->m_name = name;
    }
    t_thread_name = name;
}

// pthread_create：这是一个 POSIX 线程（pthreads）库的函数，用于创建新的线程。
// &m_thread：这是一个 pthread_t 类型的指针，表示新创建的线程 ID。
// nullptr：是线程的属性，通常设置为 nullptr 使用默认线程属性。
// &Thread::run：线程将执行的函数，这里指向 Thread 类中的静态成员函数 run。这个函数会作为线程的执行入口。
// this：传递给线程函数的参数，这里是 Thread 类的实例的指针。线程函数可以通过该指针访问和修改类的成员。
Thread::Thread(std::function<void()> cb, const std::string& name) :
    m_cb(cb), m_name(name) {
        int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);
        if(rt) {
            std::cerr << "pthread_create thread fail, rt=" << rt << " name=" << name;
            throw std::logic_error("pthread_create error");
        }
        // 等待线程函数完成初始化
        m_semaphore.wait();
    }

Thread::~Thread() {
    if(m_thread) {
        // int pthread_detach(pthread_t thread);
        // 参数：thread 是要分离的线程的线程 ID，通常是一个 pthread_t 类型的变量。
        // 返回值：
        // 成功时，返回 0。
        // 如果出错，返回错误码。
        pthread_detach(m_thread);
        m_thread = 0;
    }
}

void Thread::join() {
    // 只有在 m_thread 有效时，才会尝试调用 pthread_join 来等待线程结束。
    if(m_thread) {
        // pthread_join 是一个 POSIX 线程（pthreads）库提供的函数，它用于阻塞当前线程，直到指定的线程（m_thread）执行完毕。
        // pthread_join(m_thread, nullptr)：
        // m_thread：表示要等待的线程的线程 ID。pthread_join 会阻塞当前线程，直到 m_thread 所指向的线程结束。
        // nullptr：第二个参数是一个指向 void* 类型的指针，表示线程返回值的存储位置。如果不需要获取线程的返回值，可以传入 nullptr。
        // pthread_join 返回值 rt 为 0 表示操作成功，其他非零值表示失败。
        int rt = pthread_join(m_thread, nullptr);
        if(rt) {
            std::cerr << "pthread_join failed, rt = " << rt << ", name = " << m_name << std::endl;
            throw std::logic_error("pthread_join error");
        }
        // m_thread = 0 将线程 ID 重置为 0，表示当前线程已经结束，线程资源已经回收。
        m_thread = 0;
    }
}

void* Thread::run(void* arg) {
    Thread* thread = (Thread*)arg;

    t_thread = thread;
    t_thread_name = thread->m_name;
    thread->m_id = GetThreadId();

    // int pthread_setname_np(pthread_t thread, const char *name);
    // 作用：pthread_setname_np 是一个非标准（平台特定）函数，用于设置线程的名称。np 表示 "non-portable"，即该函数在不同平台的支持情况不同，它是 Linux 系统上的扩展函数。
    // thread：要设置名称的线程的线程 ID，通常是由 pthread_create 返回的 pthread_t 类型的值。
    // name：线程的名称，最多支持 15 个字符（包括终止符 \0）。线程名称主要用于调试和日志输出。
    // 成功时，返回 0。
    // 失败时，返回错误码。
    // pthread_self() 获取当前线程的线程 ID
    // thread->m_name.substr(0, 15) 从线程对象 thread 中获取线程名称 m_name，并且确保该名称的长度不超过 15 个字符（以防止超出系统限制）。substr(0, 15) 会从字符串 m_name 中截取前 15 个字符。
    // c_str() 将 std::string 类型的名称转换为 const char* 类型，以适应 pthread_setname_np 函数的参数要求
    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

    std::function<void()> cb;

    // swap -> 可以减少m_cb中只能指针的引用计数
    cb.swap(thread->m_cb);

    // 初始化完成
    thread->m_semaphore.signal();
    cb();
    return 0;
}

}