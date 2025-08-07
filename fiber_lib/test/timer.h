#ifndef __SYLAR_TIMER_H__
#define __SYLAR_TIMER_H__

// 智能指针头文件
#include <memory>
#include <vector>
#include <set>
// 读写锁头文件
#include <shared_mutex>
// 判断是否符号条件
#include <assert.h>
// 函数对象
#include <functional>
// 互斥锁
#include <mutex>

namespace sylar {

// 定时器管理类
class TimerManager;

// 继承的public是用来返回智能指针timer的this值
class Timer: public std::enable_shared_from_this<Timer> {
    // 设置成友元访问timerManager类的函数和成员变量
    friend class TimerManager;
public:
    // 从时间堆中删除timer
    // 取消定时任务
    bool cancel();

    // 刷新timer
    // 刷新任务，下次超时时间重置为当前时间+初始设置的间隔。
    bool refresh();

    // 重设timer的超时时间
    // 修改任务的超时时间。
    // ms定时器执行间隔时间(ms)，from_now是否从当前时间开始计算
    bool reset(uint64_t ms, bool from_now);

private:
    Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager);

private:
    // 是否循环
    // 标识任务是否为周期性的（重复执行）。
    bool m_recurring = false;

    // 超时时间
    // 超时周期（毫秒），表示任务的延迟间隔。
    uint64_t m_ms = 0;

    // 绝对超时时间, 即该定时器下一次触发的时间点。
    // 下一次任务的绝对执行时间点（精确到系统时钟）。
    std::chrono::time_point<std::chrono::system_clock> m_next;

    // 超时时触发的回调函数
    std::function<void()> m_cb;

    // 管理此timer的管理器
    TimerManager* m_manager = nullptr;

private:
    // 实现最小堆的比较函数，⽤于⽐较两个Timer对象，⽐较的依据是绝对超时时间。
    // 提供给 std::set 用于排序定时器，保证堆顶永远是最近执行的任务。

    // 重载operator()的结构体或类，我们通常称之为函数对象（Functor）。
    // 这种类的对象能够像函数一样被调用。
    struct Comparator {
        bool operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const;
    };
};

class TimerManager {
    friend class Timer;
public:
    TimerManager();
    virtual ~TimerManager();

    // 添加timer
    // ms定时器执行间隔时间
    // cb定时器回调函数
    // recurring是否循环定时器
    std::shared_ptr<Timer> addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);

    // 添加条件timer
    // 添加条件定时器，只有当weak_cond 所引用的资源还存活时，才会执行回调函数。
    std::shared_ptr<Timer> addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring = false);

    // 拿到堆中最近的超时时间
    // 获取最近一个定时任务距离当前时间的间隔（毫秒）。
    uint64_t getNextTimer();

    // 取出所有超时定时器的回调函数
    // 列出所有超时（已到期）任务的回调，供外部执行。
    void listExpiredCb(std::vector<std::function<void()>>& cbs);

    // 堆中是否有timer
    // 检测是否还有未执行的定时任务。
    bool hasTimer();

protected:
    // 当一个最早的timer加入到堆中 -> 调用该函数
    // 每次有更早的定时任务插入到堆顶时触发。
    virtual void onTimerInsertedAtFront() {};

    // 添加timer
    // 添加定时任务
    void addTimer(std::shared_ptr<Timer> timer);

private:
    // 当系统时间改变时 -> 调用该函数
    // 检测系统时钟是否发生了回退，若发生，则重新调整定时器集合。
    bool detectClockRollover();

private:
    // 用于多线程下安全地访问定时器堆的读写锁。
    std::shared_mutex m_mutex;

    // 时间堆
    // 基于红黑树的有序集合，定时任务按最近执行的顺序存储。
    // 存储所有的 Timer 对象，并使用 Timer::Comparator 进行排序，确保最早超时的 Timer 在最前面。
    std::set<std::shared_ptr<Timer>, Timer::Comparator> m_timers;

    // 在下次getNextTime()执行前 onTimerInsertedAtFront()是否已经被触发了 -> 在此过程中 onTimerInsertedAtFront()只执行一次
    // 标记在下次getNextTimer()执行前，onTimerInsertedAtFront()是否被触发过，以减少频繁通知。
    // 上次检查系统时间是否回退的绝对时间
    bool m_tickled = false;

    // 上次检查系统时间是否回退的绝对时间
    std::chrono::time_point<std::chrono::system_clock> m_previousTime;
};
}
#endif