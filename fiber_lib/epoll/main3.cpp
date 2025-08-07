#include <iostream>
#include <unistd.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <cstring>
#include <thread>
#include <vector>
#include <mutex>

#define MAX_EVENTS 256
#define READ_BUFFER_SIZE 1024

std::mutex epoll_mutex;

// Helper function to create a non-blocking TCP socket
int createNonBlockingSocket() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    return sockfd;
}

void handleRequest(int epfd, int client_fd) {
    char buffer[READ_BUFFER_SIZE];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));

    if (bytes_read <= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, client_fd, nullptr);
        close(client_fd);
        return;
    }

    std::string response = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/plain\r\n"
                           "Content-Length: 13\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "Hello, World!";

    write(client_fd, response.c_str(), response.size());

    // Close connection after responding
    epoll_ctl(epfd, EPOLL_CTL_DEL, client_fd, nullptr);
    close(client_fd);
}

void epollLoop(int epfd, int server_fd) {
    struct epoll_event events[MAX_EVENTS];

    while (true) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);

        if (n == -1) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            int event_fd = events[i].data.fd;

            if (event_fd == server_fd) {
                // Accept new connections
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);

                while (true) {
                    int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        } else {
                            perror("accept");
                            break;
                        }
                    }

                    int flags = fcntl(client_fd, F_GETFL, 0);
                    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN | EPOLLET;
                    client_ev.data.fd = client_fd;

                    std::lock_guard<std::mutex> lock(epoll_mutex);
                    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &client_ev);
                }
            } else {
                handleRequest(epfd, event_fd);
            }
        }
    }
}

void startEpollServer(int port, size_t num_threads) {
    int server_fd = createNonBlockingSocket();
    if (server_fd == -1) return;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_fd);
        return;
    }

    if (listen(server_fd, SOMAXCONN) == -1) {
        perror("listen");
        close(server_fd);
        return;
    }

    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1");
        close(server_fd);
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

    std::vector<std::thread> workers;
    for (size_t i = 0; i < num_threads; ++i) {
        workers.emplace_back(std::thread(epollLoop, epfd, server_fd));
    }

    for (auto& worker : workers) {
        worker.join();
    }

    close(epfd);
    close(server_fd);
}

int main() {
    size_t num_threads = 4;

    std::cout << "Starting epoll server on port 8081 with " << num_threads << " threads." << std::endl;

    startEpollServer(8081, num_threads);

    return 0;
}
