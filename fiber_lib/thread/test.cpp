#include <iostream>
#include <memory>
#include <vector>
#include <unistd.h>  
#include "thread.h"

using namespace sylar;

void func() {
    // Thread::GetThreadId()就是return syscall(SYS_gettid);
    // Thread::GetName()就是 t_thread_name "thread_" + std::to_string(i));
    // Thread::GetThis()->getId()就是thread->m_id = GetThreadId();
    // Thread::GetThis()->getName()就是m_name
    std::cout << "id: " << Thread::GetThreadId() << ", name: " << Thread::GetName();
    std::cout << ", this id: " << Thread::GetThis()->getId() << ", this name: " << Thread::GetThis()->getName() << std::endl;

    // unsigned int sleep(unsigned int seconds);
    // 让当前线程/进程“睡眠”60秒，即暂停执行60秒钟
    sleep(60);
}

int main() {
    std::vector<std::shared_ptr<Thread>> thrs;

    for(int i = 0; i < 5; ++i) {
        std::shared_ptr<Thread> thr = std::make_shared<Thread>(&func, "thread_" + std::to_string(i));
        thrs.push_back(thr);
    }

    for(int i = 0; i < 5; ++i) {
        thrs[i]->join();
    }
    return 0;
}