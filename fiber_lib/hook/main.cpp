#include "ioscheduler.h"
#include "hook.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <stack>
#include <cstring>
#include <chrono>
#include <thread>

// 定义了一个名为sock_listen_fd的全局静态变量，表示监听套接字的文件描述符。
// 初始值为-1（无效值，通常用于表示未初始化状态）
static int sock_listen_fd = -1;

// 函数前向声明（声明而未定义）
// 在代码中，test_accept()作为回调函数传递给IOManager::addEvent
// 此时，test_accept()还没有被定义。如果没有前向声明，编译器在编译main.cpp时就会遇到test_accept()这个未定义的函数，会报错
void test_accept();

// 错误处理函数
void error(const char* msg) {
    // 输出系统调用出错信息。
    // 会自动在msg后追加一个冒号和空格，再附加错误原因描述（由全局变量errno决定）。
    perror(msg);
    printf("erreur...\n");
    exit(1);
}

// 异步IO事件监听（核心函数）
void watch_io_read() {
    // 使用某个特定的异步IO管理器（如sylar框架的IO管理器），监听一个文件描述符上的读事件。
    // 当有新客户端连接到来时，监听套接字会变为可读状态，进而触发指定的回调函数。
    sylar::IOManager::GetThis()->addEvent(sock_listen_fd, sylar::IOManager::READ, test_accept);
}

// 新增一个函数处理客户端数据的收发
void handle_client(int fd) {
    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));

    int ret = recv(fd, buffer, sizeof(buffer), 0);
    if(ret > 0) {
        const char *response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: 13\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "Hello, World!";
        send(fd, response, strlen(response), 0);
        close(fd);
    } else if(ret == -1 && errno == EAGAIN) {
        // 数据还没准备好，重新等待
        sylar::IOManager::GetThis()->addEvent(fd, sylar::IOManager::READ, [fd]() {
            handle_client(fd);
        });
    } else {
        // 发生其他错误或者连接关闭
        close(fd);
    }
}

void test_accept() {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    while(true) {
        int fd = accept(sock_listen_fd, (struct sockaddr*)&addr, &len);

        if(fd < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // 重新监听
                sylar::IOManager::GetThis()->addEvent(sock_listen_fd, sylar::IOManager::READ, test_accept);
            } else {
                perror("accept failed");
            }
            break;
        }

        std::cout << "accepted connection, fd = " << fd << std::endl;
        fcntl(fd, F_SETFL, O_NONBLOCK);

        // 直接在IOManager线程中处理客户端请求
        sylar::IOManager::GetThis()->addEvent(fd, sylar::IOManager::READ, [fd]() {
            handle_client(fd);
        });
    }
}


// void test_accept() {
//     static std::atomic<int> next_thread_id{0};
//     int total_threads = 8;  

//     int user_thread_index = next_thread_id++ % total_threads;

//     pid_t real_thread_id = sylar::Scheduler::GetThis()->getThreadIdByIndex(user_thread_index);

//     sylar::Scheduler::GetThis()->scheduleLock([=]() {
//         struct sockaddr_in addr;
//         socklen_t len = sizeof(addr);
//         int fd = accept(sock_listen_fd, (struct sockaddr*)&addr, &len);

//         if (fd < 0) {
//             if (errno != EAGAIN && errno != EWOULDBLOCK) {
//                 perror("accept failed");
//             }
//             // 继续监听下一个连接
//             sylar::IOManager::GetThis()->addEvent(sock_listen_fd, sylar::IOManager::READ, test_accept);
//             return;
//         }
//         if(fd >= 0) {
//             fcntl(fd, F_SETFL, O_NONBLOCK);

//             sylar::IOManager::GetThis()->addEvent(fd, sylar::IOManager::READ, [fd, user_thread_index, total_threads]() {
//                 int next_thread_index = (user_thread_index + 1) % total_threads;
//                 pid_t next_real_thread_id = sylar::Scheduler::GetThis()->getThreadIdByIndex(next_thread_index);

//                 sylar::Scheduler::GetThis()->scheduleLock([fd]() {
//                     handle_client(fd);
//                 }, next_real_thread_id);
//             });
//         }

//         sylar::IOManager::GetThis()->addEvent(sock_listen_fd, sylar::IOManager::READ, test_accept);
//     }, real_thread_id);
// }

// void test_accept() {
//     sylar::Scheduler::GetThis()->scheduleLock([]() {
//         struct sockaddr_in addr;
//         memset(&addr, 0, sizeof(addr));
//         socklen_t len = sizeof(addr);

//         int fd = accept(sock_listen_fd, (struct sockaddr*)&addr, &len);

//         if(fd >= 0) {
//             std::cout << "accepted connection, fd = " << fd << std::endl;
//             fcntl(fd, F_SETFL, O_NONBLOCK);

//             sylar::IOManager::GetThis()->addEvent(fd, sylar::IOManager::READ, [fd]() {
//                 sylar::Scheduler::GetThis()->scheduleLock([fd]() {
//                     handle_client(fd);
//                 });
//             });
//         }

//         // 重新注册accept事件，确保持续监听
//         sylar::IOManager::GetThis()->addEvent(sock_listen_fd, sylar::IOManager::READ, test_accept);
//     });
// }


// 修改后的test_accept函数
// 将事件触发后的任务放入Scheduler统一管理，而不是直接在当前线程执行。
// 保证了多个线程均衡处理客户端连接与数据收发任务。
// void test_accept() {
//     struct sockaddr_in addr;
//     memset(&addr, 0, sizeof(addr));
//     socklen_t len = sizeof(addr);

//     int fd = accept(sock_listen_fd, (struct sockaddr*)&addr, &len);

//     if(fd >= 0) {
//         std::cout << "accepted connection, fd = " << fd << std::endl;
//         fcntl(fd, F_SETFL, O_NONBLOCK);

//         sylar::IOManager::GetThis()->addEvent(fd, sylar::IOManager::READ, [fd]() {
//             // 任务放入Scheduler队列，由线程池统一调度
//             sylar::Scheduler::GetThis()->scheduleLock([fd]() {
//                 handle_client(fd);
//             });
//         });
//     }

//     // 重新注册accept事件，确保循环接受新连接
//     sylar::IOManager::GetThis()->addEvent(sock_listen_fd, sylar::IOManager::READ, [](){
//         sylar::Scheduler::GetThis()->scheduleLock(test_accept);
//     });
// }

// 当监听套接字 (sock_listen_fd) 有新客户端连接请求时，被异步IO管理器自动回调
// void test_accept() {
    // 创建一个sockaddr_in结构体来存储连接客户端的信息（如IP地址、端口）
    // struct sockaddr_in addr;
    // memset(&addr, 0, sizeof(addr));
    // socklen_t len = sizeof(addr);

    // 调用accept接受客户端连接，返回新的客户端连接套接字 (fd)
    // int fd = accept(sock_listen_fd, (struct sockaddr*)&addr, &len);
    // if(fd < 0) {
        //std::cout << "accept failed, fd = " << fd << ", errno = " << errno << std::endl;
    // } else {
        // 如果成功，fd 是与客户端通信的新套接字，原监听套接字继续等待其他连接
        // std::cout << "accepted connection, fd = " << fd << std::endl;

        // 将新连接的套接字设置为非阻塞模式
        // 使用fcntl函数给新连接的套接字设置为非阻塞模式。
        // 非阻塞模式允许后续的recv、send等调用立即返回，而不会一直阻塞等待
        // fcntl(fd, F_SETFL, O_NONBLOCK);

        // 注册新连接套接字的读事件
        // 当客户端连接套接字变为可读状态（客户端发送数据）时，自动执行后面的回调函数（使用lambda函数定义匿名函数作为回调）
//         sylar::IOManager::GetThis()->addEvent(fd, sylar::IOManager::READ, [fd]() {
//             char buffer[1024];
//             memset(buffer, 0, sizeof(buffer));
//             while(true) {
//                 // lambda函数捕获了套接字描述符fd，后续用于接收和发送数据
//                 // 使用recv函数循环接收客户端发送的数据，缓冲区大小为1024字节
//                 int ret = recv(fd, buffer, sizeof(buffer), 0);
//                 if(ret > 0) {
//                     // 打印接收到的数据
//                     //std::cout << "received data, fd = " << fd << ", data = " << buffer << std::endl;
                    
//                     // 构建HTTP响应
//                     const char *response = "HTTP/1.1 200 OK\r\n"
//                                            "Content-Type: text/plain\r\n"
//                                            "Content-Length: 13\r\n"
//                                            "Connection: keep-alive\r\n"
//                                            "\r\n"
//                                            "Hello, World!";
                    
//                     // 发送HTTP响应
//                     // 调用send函数向客户端发送此HTTP响应消息
//                     // ssize_t send(int sockfd, const void *buf, size_t len, int flags);
//                     // flags: 控制发送行为的标志，通常为0表示默认发送方式
//                     ret = send(fd, response, strlen(response), 0);
//                     // std::cout << "sent data, fd = " << fd << ", ret = " << ret << std::endl;

//                     // 关闭连接
//                     // 最终调用close(fd)关闭连接套接字，结束客户端通信
//                     close(fd);
//                     break;
//                 }
//                 if(ret <= 0) {
//                     /*
//                     当调用套接字的 recv() 函数返回值为 0 时，意味着：
//                     对端（客户端）主动关闭了连接（即客户端调用了close()或shutdown()）。
//                     这种情况表示客户端不再发送任何数据，服务器也应当关闭连接
                    
//                     如果 errno == EAGAIN：
//                     说明套接字处于非阻塞模式，此时缓冲区里暂时没有数据可读，这是一种正常情况，不应关闭连接，稍后再尝试读取即可。
//                     如果 errno != EAGAIN：
//                     表示发生了某个严重的、不可恢复的错误，比如连接断开 (ECONNRESET)、网络中断等。
//                     在这种情况下，连接应该被关闭，不再继续尝试读取。
//                     */
//                     // 如果ret == 0意味着客户端已经关闭连接，服务器也关闭连接套接字
//                     if(ret == 0 || errno != EAGAIN) {
//                         /*
//                         如果 errno != EAGAIN：
//                         表示发生了某个严重的、不可恢复的错误，比如连接断开 (ECONNRESET)、网络中断等。
//                         在这种情况下，连接应该被关闭，不再继续尝试读取。
//                         */
//                         //std::cout << "closing connection, fd = " << fd << std::endl;
//                         close(fd);
//                         break;
//                     } else if(errno == EAGAIN) {
//                         // 表示当前无数据但套接字尚未关闭，稍后再试（此处注释掉了延迟重试代码）。
//                         //std::cout << "recv returned EAGAIN, fd = " << fd << std::endl;
//                         //std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 延长睡眠时间，避免繁忙等待
//                     }
//                 }
//             }
//         });
//     }
//     // 重新注册监听套接字的读事件
//     // 这样做的目的是确保服务器持续监听新的客户端连接
//     sylar::IOManager::GetThis()->addEvent(sock_listen_fd, sylar::IOManager::READ, test_accept);
// }

void test_iomanager() {
    // 服务器将监听端口 8080
    int portno = 8080;
    // 定义IPv4的地址结构体 (sockaddr_in) 存储服务器及客户端的地址信息
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // 创建服务器监听套接字
    // 调用 socket() 创建一个套接字（TCP套接字，SOCK_STREAM）。
    // 返回值保存在全局变量 sock_listen_fd 中，用于后续绑定和监听
    /*
    int socket(int domain, int type, int protocol);
    成功：返回新套接字的文件描述符 (一个非负整数)。
    失败：返回 -1，并设置 errno 来指示具体错误原因
    */
    sock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_listen_fd < 0) {
        error("Error creating socket..\n");
    }

    int yes = 1;
    // 解决 "address already in use" 错误
    // 当TCP连接关闭后，连接处于 TIME_WAIT 状态一段时间（默认约2分钟）。
    // 在此期间重新启动服务器程序，可能无法重新绑定到相同端口，导致错误 "address already in use"。
    // 解决服务器程序重启时经常遇到的端口占用错误（"address already in use"）
    // setsockopt()用于设置套接字的各种选项
    /*
    sock_listen_fd: 监听套接字的文件描述符。
    SOL_SOCKET: 套接字级别选项。
    SO_REUSEADDR: 允许地址复用，避免端口因处于TIME_WAIT状态无法立即使用。
    &yes: 选项值（开启标志）。 表示启用SO_REUSEADDR选项
    sizeof(yes): 选项值的长度
    */
    /*
    int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen);
    成功：返回值为0。
    失败：返回值为-1，且设置全局变量errno为错误码
    */
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // 初始化服务器地址结构体
    memset((char*)&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;

    // 设置服务器监听端口号，调用htons()将端口号从主机字节序转换为网络字节序（大端格式）
    /*
    网络协议（如TCP/IP）定义数据的传输必须采用统一的字节序——网络字节序（大端模式）。
    但不同计算机可能使用不同字节序（大端或小端），因此必须进行转换，确保在网络上传输的数据在任何计算机上都能正确解析。
    */
    /*
    低位字节 指的是：
    一个多字节数据中存放数值较小部分（低8位）的那个字节。
    高位字节 指的是：
    一个多字节数据中存放数值较大部分（高8位）的那个字节

    内存地址低 → 高

    大端（Big-endian）：数据的高位字节存储在低地址位置，低位字节存储在高地址位置。
    小端（Little-endian）：数据的低位字节存储在低地址位置，高位字节存储在高地址位置。
    0x1234 = 0001 0010 0011 0100
    大端模式下，数据存储方式是高位字节在前，低位字节在后
    内存地址：
    0x00  0x01   ← 高位字节
    0x01  0x23   ← 低位字节
    存储顺序：
    0x12  0x34
    0x12 是高字节，存储在低地址 0x00。
    0x34 是低字节，存储在高地址 0x01

    小端模式下，数据存储方式是低位字节在前，高位字节在后
    内存地址：
    0x00  0x34   ← 低位字节
    0x01  0x12   ← 高位字节
    存储顺序：
    0x34  0x12

    0x34 是低字节，存储在低地址 0x00。
    0x12 是高字节，存储在高地址 0x01
    */
    server_addr.sin_port = htons(portno);

    // INADDR_ANY表示服务器绑定到本机所有网络接口上。
    // 允许任意网络接口的客户端都能连接服务器（即监听0.0.0.0）
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // 绑定套接字并监听连接
    // 绑定套接字到指定地址（bind()函数）
    // 调用bind()函数将创建的服务器监听套接字 (sock_listen_fd) 与特定的IP地址和端口进行绑定。
    if(bind(sock_listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        error("Error binding socket..\n");
    }

    // 启动监听模式（listen()函数）
    // sock_listen_fd: 要监听的套接字描述符。
    // 1024: 连接请求队列的最大长度，表示可以最多同时有1024个客户端在排队等待连接。
    if(listen(sock_listen_fd, 1024) < 0) {
        error("Error listening..\n");
    }

    printf("epoll echo server listening for connections on port: %d\n", portno);

    // 设置监听套接字为非阻塞模式（fcntl()函数）
    // 配合事件驱动编程（如epoll、select或自定义事件驱动框架），能避免服务器程序在没有客户端连接时阻塞等待
    fcntl(sock_listen_fd, F_SETFL, O_NONBLOCK);
    // sylar::IOManager iom(9);

    sylar::IOManager iom(4);
    // 这里表示你将套接字sock_listen_fd的读事件交由IOManager来监听，一旦可读（新连接到来），就调用test_accept函数
    iom.addEvent(sock_listen_fd, sylar::IOManager::READ, test_accept);
}

int main(int argc, char* argv[]) {
    test_iomanager();
    return 0;
}