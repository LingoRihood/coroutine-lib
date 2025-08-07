#include "ioscheduler.h"
#include "hook.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>

static int sock_listen_fd = -1;

// 错误处理函数
void error(const char* msg) {
    perror(msg);
    printf("Error...\n");
    exit(1);
}

// 处理客户端请求并发送响应
void handle_client(int fd) {
    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));

    int ret = recv(fd, buffer, sizeof(buffer), 0);
    if (ret > 0) {
        const char *response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: 13\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "Hello, World!";
        send(fd, response, strlen(response), 0);
        close(fd);
    } else if (ret == -1 && errno == EAGAIN) {
        // 数据还没准备好，重新等待
        sylar::IOManager::GetThis()->addEvent(fd, sylar::IOManager::READ, [fd]() {
            handle_client(fd);
        });
    } else {
        // 发生其他错误或者连接关闭
        close(fd);
    }
}

// 接受连接并注册事件
void test_accept() {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    while (true) {
        int fd = accept(sock_listen_fd, (struct sockaddr*)&addr, &len);

        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 继续监听
                sylar::IOManager::GetThis()->addEvent(sock_listen_fd, sylar::IOManager::READ, test_accept);
            } else {
                perror("accept failed");
            }
            break;
        }

        std::cout << "Accepted connection, fd = " << fd << std::endl;
        fcntl(fd, F_SETFL, O_NONBLOCK);

        // 在IOManager的线程中处理客户端请求
        sylar::IOManager::GetThis()->addEvent(fd, sylar::IOManager::READ, [fd]() {
            handle_client(fd);
        });
    }
}

// 启动服务器并监听连接
void test_iomanager() {
    int portno = 8080;
    struct sockaddr_in server_addr;

    // 创建监听套接字
    sock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_listen_fd < 0) {
        error("Error creating socket..\n");
    }

    int yes = 1;
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset((char*)&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(portno);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock_listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        error("Error binding socket..\n");
    }

    if (listen(sock_listen_fd, 1024) < 0) {
        error("Error listening..\n");
    }

    printf("IOManager echo server listening on port: %d\n", portno);

    fcntl(sock_listen_fd, F_SETFL, O_NONBLOCK);

    sylar::IOManager iom(4);  // Number of threads (based on available cores)
    iom.start();

    // 注册读事件
    iom.addEvent(sock_listen_fd, sylar::IOManager::READ, test_accept);
}

int main(int argc, char* argv[]) {
    test_iomanager();
    return 0;
}