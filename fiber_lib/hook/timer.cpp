#include "timer.h"

namespace sylar {

// 取消当前的定时任务
bool Timer::cancel() {
    // std::shared_mutex 支持共享锁（读锁）和独占锁（写锁）两种模式。
    // std::unique_lock<std::shared_mutex> 即为独占锁，在锁定时，仅允许一个线程访问被保护的资源，其他线程既不能读也不能写。
    // std::shared_mutex 支持两种锁模式：
    // 共享模式（读锁）：允许多个线程同时读（用shared_lock）。
    // 独占模式（写锁）：仅允许一个线程写，禁止其他线程读写（用unique_lock）
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

    if(m_cb == nullptr) {
        // 若为 nullptr，表示已经被取消过了。
        return false;
    } else {
        m_cb = nullptr;
    }

    // 从管理器的定时器集合中移除
    // 首先调用 shared_from_this()，获得一个指向当前 Timer 对象的共享指针（shared_ptr<Timer>）。    
    // 然后调用 find()，在定时器集合 (set) 中寻找当前 Timer 对象 
    auto it = m_manager->m_timers.find(shared_from_this());
    if(it != m_manager->m_timers.end()) {
        m_manager->m_timers.erase(it);
    }
    return true;
}


// 将当前定时器的下一次执行时间重置为当前时间+初始间隔。
// 典型场景：用于定时任务需要重新计时（比如心跳检测）
bool Timer::refresh() {
    // 获取独占写锁，保护定时器管理器的集合（m_manager->m_timers）不被其他线程读写干扰。
    std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

    // 检查定时器是否有效（是否已被取消）
    if(!m_cb) {
        // 若未找到，说明定时器已经不在集合中了（可能已执行完毕或已被取消），返回false。
        return false;
    }

    // 查找自身在集合中的位置
    auto it = m_manager->m_timers.find(shared_from_this());
    if(it == m_manager->m_timers.end()) {
        return false;
    }

    // 先从集合中删除自身，便于后续重新插入到正确位置。
    m_manager->m_timers.erase(it);
    // 从当前时间点重新计时，下一次超时时间重设为当前时间加上间隔。
    m_next = std::chrono::system_clock::now() + std::chrono::milliseconds(m_ms);
    // 集合（std::set）是有序的，插入后自动按下次执行时间排序。
    m_manager->m_timers.insert(shared_from_this());
    return true;
}


// 重设当前定时器的触发周期或触发时间。
// 可以修改定时器的超时时间间隔（ms）。
// 支持从当前时间开始计时或从原有的起始时间点开始计时。
bool Timer::reset(uint64_t ms, bool from_now) {
    // 检查是否要重置
    // 如果新传入的定时周期（ms）与现有周期（m_ms）完全相同，并且不需要从当前时间重新开始计时（即from_now为false）
    if(ms == m_ms && !from_now) {
        // 代表不需要重置
        return true;
    }

    //如果不满足上面的条件需要重置，删除当前的定时器然后重新计算超时时间并重新插入定时器
    {
        std::unique_lock<std::shared_mutex> write_lock(m_manager->m_mutex);

        // 检查当前定时器的回调函数m_cb是否为空：
        // 若为空，说明该定时器已失效或被取消，此时无法重置，返回false。
        if(!m_cb) {
            return false;
        }

        // 否则就是定时器已经初始化了
        // 在集合中找到并删除自身（准备重新插入）
        auto it = m_manager->m_timers.find(shared_from_this());
        if(it == m_manager->m_timers.end()) {
            return false;
        }

        m_manager->m_timers.erase(it);
    }

    // reinsert
    // 计算新的开始计时时间点（重新插入）
    // 如果为true则重新计算超时时间，为false就需要上一次的起点开始
    // from_now == true：从当前系统时间重新开始计时。
    // from_now == false：保持原定的起始时间点（即原来的执行时间减去原本的间隔）。
    // 假设原计划12:00执行，间隔10分钟：
    // from_now==true，假设现在是11:55，则改为12:05执行。
    // from_now==false，则仍为12:00执行（可能提前或延迟，取决于新的ms）
    auto start = from_now ? std::chrono::system_clock::now(): m_next - std::chrono::milliseconds(m_ms);
    m_ms = ms;
    m_next = start + std::chrono::milliseconds(m_ms);

    // insert with lock
    // 调用管理器提供的addTimer()方法重新插入当前定时器：
    // 该方法内部同样会加锁以确保安全性。
    // 插入后，定时器集合自动重新排序，以正确反映新的执行顺序
    // 锁在删除完定时器后就立刻释放，减少锁持有时间，避免插入操作期间过长持锁，提升并发性能。
    // 再次插入时，由addTimer函数内部再单独加锁保护（粒度更细，性能更优）。
    m_manager->addTimer(shared_from_this());
    return true;
}

Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager):
    m_recurring(recurring), m_ms(ms), m_cb(cb), m_manager(manager) {
        // 记录当前时间
        auto now = std::chrono::system_clock::now();
        // 下一次超时时间
        m_next = now + std::chrono::milliseconds(m_ms);
    }

bool Timer::Comparator::operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const {
    assert(lhs != nullptr && rhs != nullptr);
    return lhs->m_next < rhs->m_next;
}

TimerManager::TimerManager() {
    //初始化当前系统事件，为后续检查系统时间错误时进行校对。
    m_previousTime = std::chrono::system_clock::now();
}

TimerManager::~TimerManager() {
}

// 创建一个新的定时器（Timer）。
// 并将其添加到TimerManager内部维护的定时器集合中
std::shared_ptr<Timer> TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring) {
    std::shared_ptr<Timer> timer(new Timer(ms, cb, recurring, this));
    // 将创建好的定时器插入到管理器的集合中进行管理。
    addTimer(timer);
    return timer;
}

// 如果条件存在 -> 执行cb()
// 在执行定时任务的回调函数 (cb) 之前，首先判断一个关联的条件对象 (weak_cond) 是否依然存在（还未被销毁）。
// 只有当条件对象存在时，才执行回调函数。
static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb) {
    //确保当前条件的的对象仍然存在
    // std::weak_ptr不能直接访问对象，必须调用lock()方法将其转化为shared_ptr：
    // 若对象还存在，lock()返回有效的shared_ptr，对象的引用计数+1，确保后续访问安全。
    // 若对象已不存在（已经销毁），则lock()返回空指针。
    std::shared_ptr<void> tmp = weak_cond.lock();
    if(tmp) {
        cb();
    }
}

// 防止回调函数访问已销毁的对象，避免内存错误和程序崩溃。
// 使用weak_ptr不增加引用计数，仅用于安全检测对象存在性，而不会延迟对象的销毁。
std::shared_ptr<Timer> TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring) {
    return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
}

// 检测定时器集合中最近（最早）的一个定时器距离当前时间还有多久会触发。
// 返回距离下一次超时触发的时间（毫秒）。
uint64_t TimerManager::getNextTimer() {
    // 使用共享锁（读锁）保护对m_timers集合的安全访问：
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    // reset m_tickled
    // 表示本次执行过getNextTimer()后，可以再次允许下一次插入定时器时重新设置该标志并进行唤醒通知。
    // 这样保证了每次调用getNextTimer()后，后续新插入最早定时器时可再次进行通知，避免通知遗漏。
    // 调用getNextTimer()意味着，事件循环（或线程）准备处理下一批定时器了。
    // 这次调用会计算距离下一个定时器触发的时间是多少
    // 重新设置m_tickled = false表示：
    // 允许再次进行下一次的唤醒通知
    m_tickled = false;

    if(m_timers.empty()) {
        // 表示当前没有任何定时任务。
        // 返回最大值
        // 返回特殊值~0ull（即无符号64位整数最大值，0xffffffffffffffff），表示没有定时器等待触发，事件循环或线程可无限等待其他事件。
        return ~0ull;
    }

    // 获取当前绝对系统时间点(now)。
    auto now = std::chrono::system_clock::now();

    // 获取最小时间堆中的第一个超时定时器判断超时
    // 获取定时器集合中第一个定时器的下一次触发时间点(time)
    auto time = (*m_timers.begin())->m_next;

    // 判断当前时间是否已经超过了下一个定时器的超时时间
    if(now >= time) {
        // 已经有timer超时
        return 0;
    } else {
        //计算从当前时间到下一个定时器超时时间的时间差，结果是一个 std::chrono::milliseconds 对象。
        // auto diff = time - now;  // diff类型为duration，但单位可能不明确
        // 为了明确将差值表示为具体单位（如毫秒），必须使用duration_cast
        // std::chrono::duration_cast<目标单位>(原始duration)
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(time - now);

        //将时间差转换为毫秒，并返回这个值。
        // count()方法用于获取duration对象中的时间间隔具体数值（整数或浮点数）。
        return static_cast<uint64_t>(duration.count());
    }
}

// 将所有已到期（超时）的定时器任务的回调函数提取出来，加入到cbs列表中等待执行。
void TimerManager::listExpiredCb(std::vector<std::function<void()>>& cbs) {
    auto now = std::chrono::system_clock::now();

    // 加写锁保护定时器集合m_timers，因为接下来要修改它（删除、重新插入）
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    // 检测系统时钟是否回退
    bool rollover = detectClockRollover();

    // 回退 -> 清理所有timer || 超时 -> 清理超时timer
    // 主体循环：清理超时定时器
    while(!m_timers.empty() && rollover || !m_timers.empty() && (*m_timers.begin())->m_next <= now) {
        std::shared_ptr<Timer> temp = *m_timers.begin();
        m_timers.erase(m_timers.begin());

        cbs.push_back(temp->m_cb);

        if(temp->m_recurring) {
            // 重新加入时间堆
            temp->m_next = now + std::chrono::milliseconds(temp->m_ms);
            m_timers.insert(temp);
        } else {
            // 清理cb
            temp->m_cb = nullptr;
        }
    }
}

bool TimerManager::hasTimer() {
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return !m_timers.empty();
}

// lock + tickle()
// 此函数将已经创建好的定时器对象（std::shared_ptr<Timer>）插入到TimerManager维护的定时器集合中。
// 插入后自动根据定时器的下一次执行时间排序。
// 如果新插入的定时器位于最前面（即下一个即将触发的定时器），则需要进行通知（唤醒）操作。
void TimerManager::addTimer(std::shared_ptr<Timer> timer) {
    // 标识插入的是最早超时的定时器
    bool at_front = false;
    {
        // 使用独占锁保护对定时器集合（m_timers）的操作，确保线程安全性。
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);

        // 将定时器插入集合，并记录是否位于最前面
        // 调用std::set容器的insert方法，向容器中插入timer。
        // std::set的insert方法原型：
        // 返回值是一个pair：
        // std::pair<iterator, bool> insert(const value_type& value);
        // first: 指向被插入元素的迭代器
        // second: bool类型，插入是否成功，true表示成功，false表示集合中已存在该元素
        //将定时器插入到 m_timers 集合中。由于 m_timers 是一个 std::set，插入时会自动按定时器的超时时间排序。
        auto it = m_timers.insert(timer).first;

        // 如果插入位置位于集合开头，说明该定时器是下一个将触发的最早定时器。
        // 并且此时还未通知过（m_tickled == false），那么需通知相关线程唤醒检查。
        at_front = (it == m_timers.begin()) && !m_tickled;

        // only tickle once till one thread wakes up and runs getNextTime()
        // //标识有一个新的最早定时器被插入了，防止重复唤醒。
        if(at_front) {
            m_tickled = true;
        }
    }

    // 如果通知函数(onTimerInsertedAtFront)的执行较慢或有额外逻辑，放在锁内会增加锁的持有时间，降低并发性能。
    // 放在锁外释放锁后执行，可以明显提高程序的并发效率。
    if(at_front) {
        // wake up 
        // 虚函数具体执行在ioscheduler
        // 通知或唤醒可能正在睡眠或等待的线程，以便及时处理新的最早定时器
        onTimerInsertedAtFront();
    }
}

// 检测系统时间是否出现了“回滚”（Clock Rollover）现象。
bool TimerManager::detectClockRollover() {
    // 初始化一个布尔变量 rollover，默认表示系统时间未回退。
    bool rollover = false;
    auto now = std::chrono::system_clock::now();

    // 60 * 60 * 1000 毫秒 = 1小时。
    // 当前系统时间比上一轮时间小了超过 1 小时
    // 如果满足这个条件，就说明发生了系统时钟异常回退（rollover），设置 rollover = true。
    if(now < (m_previousTime - std::chrono::milliseconds(60 * 60 * 1000))) {
        rollover = true;
    }
    m_previousTime = now;
    return rollover;
}
}