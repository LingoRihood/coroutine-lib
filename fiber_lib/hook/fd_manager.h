#ifndef _FD_MANAGER_H_
#define _FD_MANAGER_H_

#include <memory>
#include <shared_mutex>
#include "thread.h"

namespace sylar {
// fd info
class FdCtx: public std::enable_shared_from_this<FdCtx> {
private:
    //标记文件描述符是否已初始化。
    bool m_isInit = false;

    //标记文件描述符是否是一个套接字。
    bool m_isSocket = false;

    //标记文件描述符是否设置为系统非阻塞模式
    bool m_sysNonblock = false;

    //标记文件描述符是否设置为用户非阻塞模式
    bool m_userNonblock = false;

    //标记文件描述符是否已关闭。
    bool m_isClosed = false; 

    //文件描述符的整数值
    int m_fd;

    // read event timeout
    //读事件的超时时间，默认为 -1 表示没有超时限制。
    uint64_t m_recvTimeout = (uint64_t)-1;

    // write event timeout
    //写事件的超时时间，默认为 -1 表示没有超时限制。
    uint64_t m_sendTimeout = (uint64_t)-1;

public:
    FdCtx(int fd);
    ~FdCtx();

    //初始化 FdCtx 对象。
    bool init();
    bool isInit() const {
        return m_isInit;
    }

    bool isSocket() const {
        return m_isSocket;
    }

    bool isClosed() const {
        return m_isClosed;
    }

    //设置和获取用户层面的非阻塞状态。
    void setUserNonblock(bool v) {
        m_userNonblock = v;
    }

	bool getUserNonblock() const {
        return m_userNonblock;
    }

    //设置和获取系统层面的非阻塞状态。
	void setSysNonblock(bool v) {
        m_sysNonblock = v;
    }

	bool getSysNonblock() const {
        return m_sysNonblock;
    }

    //设置和获取超时时间，type 用于区分读事件和写事件的超时设置，v表示时间毫秒。
	void setTimeout(int type, uint64_t v);
	uint64_t getTimeout(int type);
};

// 文件描述符管理器，维护多个FdCtx对象，并提供对fd上下文的查询、创建、删除功能。
class FdManager {
public:
    //构造函数
    //获取指定文件描述符的 FdCtx 对象。如果 auto_create 为 true，在不存在时自动创建新的 FdCtx 对象。
    FdManager();
    std::shared_ptr<FdCtx> get(int fd, bool auto_create = false);

    //删除指定文件描述符的 FdCtx 对象  
    void del(int fd);

private:
    //用于保护对 m_datas 的访问，支持共享读锁和独占写锁。
    // std::shared_mutex m_mutex;
    // std::recursive_mutex允许同一线程多次获得锁，而不会造成死锁
    std::recursive_mutex m_mutex;

    //存储所有 FdCtx 对象的共享指针。
    std::vector<std::shared_ptr<FdCtx>> m_datas;
};

template<typename T>
class Singleton {
private:
    //对外提供的实例
    static T* instance;
    //互斥锁
    static std::mutex mutex;

protected:
    Singleton() {}

public:
    // Delete copy constructor and assignment operation
    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;

    static T* GetInstance() {
        if (instance == nullptr) {
            std::lock_guard<std::mutex> lock(mutex);  // 线程安全
            if (instance == nullptr) {
                instance = new T();
            }
        }
        //提高对外的访问点，在系统生命周期中
        //一般一个类只有一个全局实例。
        return instance;
    }

    static void DestroyInstance() {
        std::lock_guard<std::mutex> lock(mutex);
        delete instance;
        //防止野指针
        instance = nullptr;
    }
};

//重定义将Singleton<FdManager> 变成FdMgr的缩写。 
typedef Singleton<FdManager> FdMgr;
}
#endif