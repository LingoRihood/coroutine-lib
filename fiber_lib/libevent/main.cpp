#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/thread.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <iostream>
#include <sstream>
#include <netinet/tcp.h>


#define PORT 8082
#define THREAD_COUNT 4
#define BACKLOG 128

struct Worker {
    struct event_base* base = nullptr;
    struct event* notify_event = nullptr;
    int notify_receive_fd = -1;
    int notify_send_fd = -1;
    std::thread thread;
};

std::vector<Worker> workers(THREAD_COUNT);
size_t current_worker = 0;

void handle_request(struct bufferevent* bev, void* ctx) {
    char buffer[1024];
    int n = bufferevent_read(bev, buffer, sizeof(buffer) - 1);
    if (n <= 0) return;
    buffer[n] = '\0';

    std::string request(buffer);
    if (request.find("Connection: keep-alive") != std::string::npos ||
        request.find("Connection: Keep-Alive") != std::string::npos) {
        // Send keep-alive response
        std::string response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: 13\r\n"
                               "Connection: keep-alive\r\n"
                               "\r\n"
                               "Hello, World!";
        bufferevent_write(bev, response.c_str(), response.size());
    } else {
        // Default to closing connection
        std::string response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: 13\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "Hello, World!";
        bufferevent_write(bev, response.c_str(), response.size());
        bufferevent_flush(bev, EV_WRITE, BEV_FINISHED);
        bufferevent_free(bev); // Close after response
    }
}

void event_cb(struct bufferevent* bev, short events, void* ctx) {
    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        bufferevent_free(bev);
    }
}

void worker_loop(Worker* worker) {
    event_base_dispatch(worker->base);
}

void notify_cb(evutil_socket_t fd, short what, void* arg) {
    Worker* worker = (Worker*)arg;
    int client_fd;
    read(fd, &client_fd, sizeof(client_fd));

    int one = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct bufferevent* bev = bufferevent_socket_new(worker->base, client_fd, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(bev, handle_request, nullptr, event_cb, nullptr);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
}

void accept_cb(struct evconnlistener* listener, evutil_socket_t fd, struct sockaddr* sa, int socklen, void* arg) {
    Worker* worker = &workers[current_worker];
    current_worker = (current_worker + 1) % THREAD_COUNT;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    write(worker->notify_send_fd, &fd, sizeof(fd));
}

int main() {
    evthread_use_pthreads();

    // 初始化工作线程结构并启动线程
    for (int i = 0; i < THREAD_COUNT; ++i) {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            perror("socketpair");
            return 1;
        }

        workers[i].notify_receive_fd = fds[0];
        workers[i].notify_send_fd = fds[1];
        workers[i].base = event_base_new();

        workers[i].notify_event = event_new(
            workers[i].base,
            workers[i].notify_receive_fd,
            EV_READ | EV_PERSIST,
            notify_cb,
            &workers[i]);
        event_add(workers[i].notify_event, nullptr);

        workers[i].thread = std::thread(worker_loop, &workers[i]);
    }

    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(PORT);

    struct event_base* main_base = event_base_new();

    struct evconnlistener* listener = evconnlistener_new_bind(
        main_base, accept_cb, nullptr,
        LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE,
        BACKLOG, (struct sockaddr*)&sin, sizeof(sin));

    std::cout << "Libevent server running on port " << PORT << " with " << THREAD_COUNT << " threads." << std::endl;
    event_base_dispatch(main_base);

    evconnlistener_free(listener);
    event_base_free(main_base);
    for (auto& worker : workers) {
        close(worker.notify_receive_fd);
        close(worker.notify_send_fd);
        if (worker.thread.joinable()) {
            worker.thread.join();
        }
        if (worker.notify_event) event_free(worker.notify_event);
        if (worker.base) event_base_free(worker.base);
    }
    return 0;
}
