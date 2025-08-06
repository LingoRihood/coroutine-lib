#include "fd_manager.h"
#include "hook.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sylar {
// instantiate
//FdManager 类有一个全局唯一的单例实例。
// Static variables need to be defined outside the class
//这些行代码定义了 Singleton 类模板的静态成员变量 instance 和 mutex。静态成员变量需要在类外部定义和初始化。
// 编译器马上为Singleton<FdManager>生成代码（如构造函数、GetInstance()方法、静态成员定义等）。
// 后续其他文件再次使用该模板时，不再生成新代码，而直接引用已经生成好的版本。
// 编译链接过程更快速、更高效，二进制文件体积更小
// 这里的class关键词不是定义类，而是在模板实例化语法中固定的关键词，告诉编译器现在要实例化的是一个模板类。这样，程序在其他.cpp文件中再使用时，不会重新生成这些代码，而只引用已经生成的实例。
/*
template
告诉编译器：接下来是一个模板相关的定义或实例化。
class
这里的关键字class并非在定义新类，而是明确告诉编译器：“接下来实例化的是一个模板类”。
Singleton<FdManager>
要实例化的模板类名（模板类Singleton，实例化类型为FdManager）。

通俗地讲，这行代码的意思是：
编译器，请立即生成模板类Singleton以FdManager为模板参数的全部代码，不要等我用到时才生成。

默认情况下，模板类只有被用到时才生成具体代码。
当你显式写出这句，编译器就提前为你准备好了Singleton<FdManager>所有的成员函数、静态变量、构造函数、析构函数等具体代码。
*/
template class Singleton<FdManager>;
/*
不显式实例化：
多个.cpp文件中使用Singleton<FdManager>
   ↓↓
编译阶段各自生成Singleton<FdManager>代码
   ↓↓
链接时合并多个重复的Singleton<FdManager>代码
   ↓↓
程序体积增大，链接时间增长

显式实例化：
模板类定义Singleton<T>
   ↓↓（立即实例化）
template class Singleton<FdManager>; 
   ↓↓
只生成一次Singleton<FdManager>代码
   ↓↓
后续使用直接引用此代码，无需再次生成
*/

// Static variables need to be defined outside the class
// 静态成员变量必须有一个单独的、类外的定义，否则链接时会报错。
template<typename T>
T* Singleton<T>::instance = nullptr;

template<typename T>
std::mutex Singleton<T>::mutex;

FdCtx::FdCtx(int fd):m_fd(fd) {
    init();
}

FdCtx::~FdCtx() {
    // Destructor implementation needed
}

bool FdCtx::init() {
    // 如果已经初始化过了就直接返回 true
    if(m_isInit) {
        return true;
    }

    struct stat statbuf;
    
    // fd is in valid
    // fstat 函数用于获取与文件描述符 m_fd 关联的文件状态信息存放到 statbuf 中。如果 fstat() 返回 -1，表示文件描述符无效或出现错误。
/*
int fstat(int fd, struct stat *buf);
fd：你想检查的文件描述符。
buf：一个stat结构的指针，调用成功后fd的信息会填充进去。
成功时返回0；
出错时返回-1（说明fd无效、已关闭或者其它错误）。

S_ISSOCK()宏：
作用：判断给定的文件是否为套接字类型。
用法：S_ISSOCK(statbuf.st_mode)
返回值为非0值表示是socket类型，否则不是。
    */
    if(-1 == fstat(m_fd, &statbuf)) {
        m_isInit = false;
        m_isSocket = false;
    } else {
        // S_ISSOCK(statbuf.st_mode) 用于判断文件类型是否为套接字
        m_isInit = true;
        m_isSocket = S_ISSOCK(statbuf.st_mode);
    }

    // if it is a socket -> set to nonblock
    // 如果fd是socket，强制设置为非阻塞模式
    // 通常网络应用程序（如高性能服务器）都使用非阻塞socket进行高效的IO处理，避免IO操作阻塞程序执行。
    if(m_isSocket) {
        // 表示 m_fd 关联的文件是一个套接字：
        // fcntl_f() -> the original fcntl() -> get the socket info
        // 获取文件描述符的状态
/*
fcntl()用于修改文件描述符(fd)的属性或获取属性。
此处使用两种cmd值：
F_GETFL：获取文件描述符的标志（当前状态）。
F_SETFL：设置文件描述符的新状态标志。
当cmd为F_GETFL或F_GETFD时，该参数可忽略（通常传0）。
O_NONBLOCK标志表示非阻塞模式。
*/
        int flags = fcntl(m_fd, F_GETFL, 0);

        // 检查当前标志中是否已经设置了非阻塞标志。如果没有设置：
        if(!(flags & O_NONBLOCK)) {
            // if not -> set to nonblock
            // 如果没有非阻塞模式，则强制设置非阻塞模式
            fcntl_f(m_fd, F_SETFL, flags | O_NONBLOCK);
        }
        // hook 非阻塞设置成功
        m_sysNonblock = true;
    } else {
        // 如果不是一个 socket 那就没必要设置非阻塞了。
        m_sysNonblock = false;
    }
    // 即初始化是否成功
    return m_isInit;
}

/*
接收超时 (SO_RCVTIMEO)
发送超时 (SO_SNDTIMEO)
*/
// setTimeout 函数用于设置套接字（socket）相关的超时时间（timeout），可设置两种超时类型
//type指定超时类型的标志。可能的值包括 SO_RCVTIMEO 和 SO_SNDTIMEO，分别用于接收超时和发送超时。v代表设置的超时时间，单位是毫秒或者其他。
void FdCtx::setTimeout(int type, uint64_t v) {
    //如果type类型的读事件，则超时事件设置到recvtimeout上，否则就设置到sendtimeout上。
    if(type == SO_RCVTIMEO) {
        m_recvTimeout = v;
    } else {
        m_sendTimeout = v;
    }
}

//同理根据type类型返回对应读或写的超时时间。
uint64_t FdCtx::getTimeout(int type) {
    if(type == SO_RCVTIMEO) {
        return m_recvTimeout;
    } else {
        return m_sendTimeout;
    }
}

FdManager::FdManager() {
    m_datas.resize(64);
}

std::shared_ptr<FdCtx> FdManager::get(int fd, bool auto_create) {
    if(fd == -1) {
        //文件描述符无效则直接返回。
        return nullptr;
    }

    // std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    //如果 fd 超出了 m_datas 的范围，并且 auto_create 为 false，则返回 nullptr，表示没有创建新对象的需求。
    /*
        bool auto_create：
        是否在找不到对应的 FdCtx 对象时自动创建。
        为 true 时找不到就创建。
        为 false 时找不到就返回空指针。
        */
    if ((int)m_datas.size() > fd) {
        if (m_datas[fd] || !auto_create) {
            return m_datas[fd];
        }
    }

    if (!auto_create) {
        return nullptr;
    }

    //当fd的大小超出m_data.size的值也就是m_datas[fd]数组中没找到对应的fd并且auto_create为true时候会走到这里。
    // 由于接下来要修改数据，因此必须释放读锁，避免死锁。
    // read_lock.unlock();

    // 加写锁，修改数据：
    // std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    // 扩容并创建新的FdCtx
    m_datas.resize(fd * 1.5);
    if (!m_datas[fd]) {
        m_datas[fd] = std::make_shared<FdCtx>(fd);
    }
    return m_datas[fd];
}

void FdManager::del(int fd) {
    // std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    // if(m_datas.size() <= fd) {
    //     return;
    // }
    // m_datas[fd].reset();
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if ((int)m_datas.size() > fd) {
        m_datas[fd].reset();
    }
}

}