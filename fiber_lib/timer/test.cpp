#include "timer.h"
#include <unistd.h>
#include <iostream>

using namespace sylar;

void func(int i) {
    std::cout << "i: " << i << std::endl;
}

int main(int argc, char const* argv[]) {
    // 创建了一个定时器管理器manager，使用智能指针进行内存管理
    std::shared_ptr<TimerManager> manager(new TimerManager());
    std::vector<std::function<void()>> cbs;

    // 测试listExpiredCb超时功能
    {
        // 创建多个一次性定时器
        for(int i = 0; i < 10; ++i) {
            // 每个定时器延迟时间依次增加1秒，即：第一个1秒后触发，第二个2秒后触发，依次类推直到第十个10秒后触发
            // 定时器回调函数绑定了func(i)，打印当前定时器编号i。
            // false表示这是一次性定时器，不重复触发。
            manager->addTimer((i + 1) * 1000, std::bind(&func, i), false);
        }
        std::cout << "all timers have been set up" << std::endl;

        sleep(5);
        manager->listExpiredCb(cbs);
        while(!cbs.empty()) {
            std::function<void()> cb = *cbs.begin();
            cbs.erase(cbs.begin());
            cb();
        }

        sleep(5);
        manager->listExpiredCb(cbs);
        while(!cbs.empty()) {
            std::function<void()> cb = *cbs.begin();
            cbs.erase(cbs.begin());
            cb();
        }
    }

    // 测试recurring
    // 测试重复性定时器（recurring）
    {
        manager->addTimer(1000, std::bind(&func, 1000), true);
        int j = 10;
        while(j-- > 0) {
            sleep(1);
            manager->listExpiredCb(cbs);
            while(!cbs.empty()) {
                std::function<void()> cb = *cbs.begin();
                cbs.erase(cbs.begin());
                cb();
            }
        }
    }
    return 0;
}