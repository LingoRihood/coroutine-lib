#include "ioscheduler.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <cstring>
#include <cerrno>

// 1. 构造服务器地址 159.75.118.34:80
// 2. 创建 TCP socket（非阻塞）
// 3. 调用 connect() 发起连接
// 4. epoll 注册：
//    ├─ WRITE → func2()：连接建立时发请求
//    └─ READ  → func() ：收到响应时读取数据
// 5. IOManager（基于 epoll）异步调度
// 6. 网络通信完成

using namespace sylar;

// 接收缓冲区
char recv_data[4096];

// 是一个 HTTP 请求报文，表示向服务器请求主页 
// GET / HTTP/1.0\r\n     ← 请求行：GET方法，请求"/"，协议HTTP/1.0
// \r\n                   ← 空行：表示没有额外请求头，请求结束
const char data[] = "GET / HTTP/1.0\r\n\r\n"; 

// 全局 socket fd
int sock;

void func() {
    // ssize_t recv(int sockfd, void *buf, size_t len, int flags);
    // sockfd: socket 文件描述符（int）
    // buf: 接收缓冲区（char 数组）
    // 希望读取的最大字节数（最多读 4096 字节）
    // flags 参数，设置为 0 表示默认行为（阻塞读、无特殊标志）
    // 返回值：
    // >0 : 成功读取的字节数（可能少于 4096）
    // =0 : 对端已关闭连接（FIN），表示连接结束
    // <0 : 发生错误，需通过 errno 判断原因
    // 从 sock 表示的 TCP 连接中尝试读取最多 4096 字节的数据
    // 读取到的数据会被存入 recv_data 指向的缓冲区
    // 调用会阻塞当前线程/协程，直到至少有一个字节可以读（前提是 socket 是阻塞模式）；
    // 如果 socket 是非阻塞，可能会立即返回 -1 并设置 errno = EAGAIN 或 EWOULDBLOCK
    ssize_t n = recv(sock, recv_data, 4096, 0);
    if (n > 0) {
        recv_data[n] = '\0';  // 添加终止符
        std::cout << "Received:\n" << recv_data << std::endl;
    } else if (n == 0) {
        std::cout << "Connection closed by peer\n";
    } else {
        std::cerr << "recv failed: " << strerror(errno) << std::endl;
    }
}

void func2() {
    // ssize_t send(int sockfd, const void *buf, size_t len, int flags);
    // sockfd: 目标 socket 的文件描述符，表示一个已连接的 TCP 连接
    // buf: 指向要发送的数据缓冲区的指针
    // len: 要发送的字节数，这里是 sizeof("GET / HTTP/1.0\r\n\r\n")
    // flags: flags 参数，设为 0 表示默认发送行为（阻塞发送、无特殊选项）
    // 返回值：
    // 实际成功写入到 socket 缓冲区的字节数。
    // 成功时：> 0，表示发送了多少字节；
    // 失败时：< 0，表示出错，需要检查 errno
    ssize_t n = send(sock, data, sizeof(data), 0);
    if (n < 0) {
        std::cerr << "send failed: " << strerror(errno) << std::endl;
    } else {
        std::cout << "Sent " << n << " bytes" << std::endl;
    }
}

int main(int argc, char const* argv[]) {
    IOManager manager(2);

    // 创建 socket
    // int socket(int domain, int type, int protocol);
    // AF_INET —— 地址族（domain）
    // 表示使用 IPv4 协议；
    // 如果你想用 IPv6，可以写 AF_INET6；
    // 对于本地通信（如进程间通信），可以使用 AF_UNIX
    // SOCK_STREAM —— 套接字类型（type）
    // 表示使用 面向连接的 TCP 协议
    // 0 —— 协议（protocol）
    // 一般设置为 0，让系统自动选择默认协议：
    // 如果你选了 SOCK_STREAM，就默认是 TCP；
    // 如果你选了 SOCK_DGRAM，就默认是 UDP
    // 返回值：一个整数，表示 socket 的文件描述符（fd）
    // 成功：返回一个正整数（文件描述符），用于后续的：connect()、send()、recv() 等操作；
    // 失败：返回 -1，并设置 errno 来指示错误原因（如文件描述符耗尽、权限问题等）
    sock = socket(AF_INET, SOCK_STREAM, 0);

    // sockaddr_in：IPv4 专用的 socket 地址结构
    sockaddr_in server;

    // server.sin_family = AF_INET;
    // 指定地址族为 IPv4
    server.sin_family = AF_INET;

    // 设置目标端口为 80（HTTP 默认端口）。
    // htons：将主机字节序转换为网络字节序（大端）
    server.sin_port = htons(8080);

    // 将 IP 地址字符串 "159.75.118.34" 转换为整数，并赋值给 s_addr 字段。
    // inet_addr 返回的是网络字节序的 IPv4 地址。
    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    // server.sin_addr.s_addr = inet_addr("10.1.12.15");

    // 设置 socket 为非阻塞模式
    fcntl(sock, F_SETFL, O_NONBLOCK);

    // 发起连接（非阻塞 connect）
    // 因为 socket 是非阻塞的，这通常会立即返回 -1，errno = EINPROGRESS，表示“连接正在建立中”
    int rt = connect(sock, (struct sockaddr*)&server, sizeof(server));

    if (rt == 0) {
        std::cout << "Connect immediately succeeded (rare)\n";
        func2();  // 如果立即连接成功，直接调用写回调
    } else if (rt < 0 && errno == EINPROGRESS) {
        std::cout << "Connecting...\n";
        /*
        epoll_wait 检测到 WRITE (EPOLLOUT) 事件
        │
        └── IOManager::idle()
            └── FdContext::triggerEvent(WRITE)
                └── Scheduler::scheduleLock(cb)
                    └── Scheduler::run()
                        └── Fiber::resume()
                            └── 调用你的回调函数func2()
        */
        manager.addEvent(sock, IOManager::WRITE, &func2);  // 等连接完成再写
    } else {
        std::cerr << "connect error: " << strerror(errno) << std::endl;
        close(sock);
        return -1;
    }

    // manager.addEvent(sock, IOManager::WRITE, &func2);
    // 注册读事件（无论写是否成功）
    manager.addEvent(sock, IOManager::READ, &func);

    std::cout << "event has been posted\n\n";
	return 0;
}