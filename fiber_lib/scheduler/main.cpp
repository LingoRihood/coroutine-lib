#include "scheduler.h"

using namespace sylar;

// 用于标识任务编号（从0开始递增）
static unsigned int test_number;
// 防止多个线程同时输出到控制台，导致日志混乱
std::mutex mutex_cout;

void task() {
    {
        std::lock_guard<std::mutex> lock(mutex_cout);
        // return syscall(SYS_gettid);
        std::cout << "task " << test_number++ << " is under processing in thread: " << Thread::GetThreadId() << std::endl;
    }
    sleep(1);
}

int main(int argc, char const* argv[]) {
    {
        // 可以尝试把false 变为true 此时调度器所在线程也将加入工作线程
        std::shared_ptr<Scheduler> scheduler = std::make_shared<Scheduler>(3, true, "scheduler_1");
        scheduler->start();

        // sleep(6);

        // 让主线程休眠 2 秒，等待调度器线程初始化。
        sleep(2);

        std::cout << "\nbegin post\n\n"; 

        // 使用一个循环创建了 5 个协程任务（Fiber），每个协程任务执行 task() 函数。
        for(int i = 0; i < 5; ++i) {
            std::shared_ptr<Fiber> fiber = std::make_shared<Fiber>(task);
            // std::cout <<"none" << std::endl;
            scheduler->scheduleLock(fiber);
        }

        sleep(6);
        std::cout << "\npost again\n\n"; 
        for(int i = 0; i < 15; ++i) {
            std::shared_ptr<Fiber> fiber = std::make_shared<Fiber>(task);
            scheduler->scheduleLock(fiber);
        }

        // sleep(20);
        sleep(3);

        // scheduler如果有设置将加入工作处理
        scheduler->stop();
    }
    return 0;
}