#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#define MAX_EVENTS 256
#define PORT 8888

// listen_fd: 监听套接字，用于监听新的客户端连接。
// conn_fd: 连接套接字，用于与客户端通信。
// epoll_fd: epoll 实例文件描述符。
// events[]: 存储epoll返回的事件列表。
// event: 用于设置待监听的单个事件。
int main() {
    int listen_fd, conn_fd, epoll_fd, event_count;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    struct epoll_event events[MAX_EVENTS], event;

    // 创建监听套接字
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        return -1;
    }

    int yes = 1;
    // 解决 "address already in use" 错误
    // 允许端口重用
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // 设置服务器地址和端口
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // 绑定监听套接字到服务器地址和端口
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        return -1;
    }

    // 监听连接
    if (listen(listen_fd, 1024) == -1) {
        perror("listen");
        return -1;
    }

    /*
    listen_fd（负责监听连接）  
        |__ conn_fd1（客户端1）
        |__ conn_fd2（客户端2）
        |__ conn_fd3（客户端3）
    */

    // 创建 epoll 实例
    if ((epoll_fd = epoll_create1(0)) == -1) {
        perror("epoll_create1");
        return -1;
    }

    // 添加监听套接字到 epoll 实例中
    event.events = EPOLLIN;
    event.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) == -1) {
        perror("epoll_ctl");
        return -1;
    }

    while (1) {
        // 等待事件发生
        // 无限循环等待事件发生，epoll_wait 会阻塞，直到有事件发生
        // 如果 timeout = -1，表示一直阻塞直到有事件发生。
        // 如果 timeout = 0，表示立即返回（非阻塞）。
        // 如果 timeout > 0，表示最多阻塞这么多毫秒
        // event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, 5000);
        event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        // 返回值含义：
        // > 0 表示触发事件的数量。
        // = 0 表示超时，未有事件发生。
        // < 0 出错，需检查errno确定错误原因。
        if (event_count == -1) {
            perror("epoll_wait");
            return -1;
        }

        // 处理事件
        for (int i = 0; i < event_count; i++) {
            // events[i].data.fd 存储了产生事件的文件描述符
            // listen_fd 是用于监听新连接的文件描述符
            // 当它们相等时，意味着当前事件是客户端发起了新的连接请求
            // 若事件为 新连接请求，则使用 accept() 接收新连接。
            if (events[i].data.fd == listen_fd) {
                // 有新连接到达
                // 接受新的客户端连接请求。
                // 创建一个新的套接字 (conn_fd) 专门用于与当前这个客户端通信
                conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
                if (conn_fd == -1) {
                    perror("accept");
                    continue;
                }

                // 将新连接的套接字添加到 epoll 实例中
                // 注册新套接字到epoll监听列表
                event.events = EPOLLIN;
                event.data.fd = conn_fd;

                // epoll_ctl()函数就是用于向epoll实例中添加一个新的套接字进行监听的
                // 新连接套接字 (conn_fd) 注册到epoll后，后续该客户端的数据发送都会被epoll通知。
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &event) == -1) {
                    perror("epoll_ctl");
                    return -1;
                }
            } else {
                // 处理客户端数据（已有连接）
                // 若事件为 客户端数据到达，则读取数据并返回HTTP响应。
                // 有数据可读
                char buf[1024];

                // 使用 read() 从客户端读取数据。
                int len = read(events[i].data.fd, buf, sizeof(buf) - 1);
                if (len <= 0) {
                    // 出错或连接关闭，直接关闭套接字
                    close(events[i].data.fd);
                } else {
                    // 读取到数据，向客户端发送HTTP响应
                    const char *response = "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: text/plain\r\n"
                                           "Content-Length: 1\r\n"
                                           "Connection: keep-alive\r\n"
                                           "\r\n"
                                           "1";
                    // events[i].data.fd 是当前已连接客户端的 套接字描述符
                    write(events[i].data.fd, response, strlen(response));

                    // 发送响应后，将套接字从epoll监听列表中删除
                    epoll_ctl(epoll_fd,EPOLL_CTL_DEL,events[i].data.fd,NULL);//出现70007的错误再打开，或者试试-r命令

                    // 关闭连接
                    close(events[i].data.fd);
                }
            }
        }
    }

    // 循环结束与资源释放（正常不会到达）
    // 关闭监听套接字和 epoll 实例
    close(listen_fd);
    close(epoll_fd);
    return 0;
}