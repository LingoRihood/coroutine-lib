#include "fiber.h"
#include <vector>

using namespace sylar;

class Scheduler {
public:
    // 添加协程调度任务
    void schedule(std::shared_ptr<Fiber> task) {
        m_tasks.push_back(task);
    }

    // 执行调度任务
    void run() {
        std::cout << " number " << m_tasks.size() << std::endl;
        std::shared_ptr<Fiber> task;
        auto it = m_tasks.begin();
        while(it != m_tasks.end()) {
            // 迭代器本身也是指针
            task = *it;
            // 由主协程切换到子协程，子协程函数运行完毕后自动切换到主协程
            // 每次 resume() 被调用时，当前协程会从 READY 状态切换到 RUNNING，执行 test_fiber(i)。
            // 执行完毕后，调用 yield()，控制权会回到调度器或主协程。
            // 然后调度器继续从任务队列中取出下一个协程任务执行。
            task->resume();
            it++;
        }
        // std::cout << "开始析构"<< std::endl;
        m_tasks.clear();
        // 当 shared_ptr 离开作用域时，它会自动销毁并减少引用计数。如果没有其他 shared_ptr 引用该对象，那么引用计数降为 0，对象会被销毁。
    }
private:
    // 任务队列
    std::vector<std::shared_ptr<Fiber>> m_tasks;
};

void test_fiber(int i) {
    std::cout << "Richard Come!" << i << std::endl;
}

int main() {
    // 初始化当前线程的主协程
    // 这行代码会初始化当前线程的主协程。通常，主协程是线程的默认协程，所有其他协程的切换都依赖于它。Fiber::GetThis() 会返回当前主协程的 shared_ptr。
    Fiber::GetThis();

    // 创建调度器
    Scheduler sc;

    // 添加调度任务（任务和子协程绑定）
    // 这段代码创建了 20 个协程，每个协程执行 test_fiber 函数并传入不同的参数 i。每个 std::shared_ptr<Fiber> 都是一个任务对象，它会被添加到调度器的任务队列中
    for(auto i = 0; i < 20; ++i) {
        // 创建子协程
        // 使用共享指针自动管理资源 -> 过期后自动释放子协程创建的资源
        // bind函数 -> 绑定函数和参数用来返回一个函数对象
        // std::bind 是一个 C++ 标准库中的函数，用来将一个函数或可调用对象和参数绑定在一起，返回一个新的可调用对象
        std::shared_ptr<Fiber> fiber = std::make_shared<Fiber>(std::bind(test_fiber, i), 0, false);
        sc.schedule(fiber);
    }

    // 执行调度任务
    sc.run();
    return 0;
}