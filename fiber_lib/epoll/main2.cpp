#include <iostream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <vector>

#define MAX_EVENTS 1024
#define BUFFER_SIZE 1024
#define THREADS 4

int create_server_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    listen(sockfd, 1024);

    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    return sockfd;
}

void handle_client(int epfd, int fd) {
    char buffer[BUFFER_SIZE];
    int n = recv(fd, buffer, BUFFER_SIZE, 0);
    if (n <= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
    } else {
        const char response[] = "HTTP/1.1 200 OK\r\n"
                                "Content-Length: 13\r\n"
                                "Connection: close\r\n"
                                "\r\n"
                                "Hello, World!";
        send(fd, response, sizeof(response) - 1, 0);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
    }
}

void worker(int listen_fd) {
    int epfd = epoll_create1(0);

    epoll_event ev{}, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;

    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    while (true) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == listen_fd) {
                sockaddr_in client_addr{};
                socklen_t len = sizeof(client_addr);
                int conn_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &len);
                if (conn_fd >= 0) {
                    int flags = fcntl(conn_fd, F_GETFL, 0);
                    fcntl(conn_fd, F_SETFL, flags | O_NONBLOCK);

                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = conn_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &ev);
                }
            } else {
                handle_client(epfd, events[i].data.fd);
            }
        }
    }
}

int main() {
    int port = 8888;
    int listen_fd = create_server_socket(port);

    std::cout << "Server started on port " << port << std::endl;

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker, listen_fd);
    }

    for (auto& th : threads) {
        th.join();
    }

    close(listen_fd);
    return 0;
}