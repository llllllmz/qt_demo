#ifndef SIGNAL_SLOT_DEMO_H
#define SIGNAL_SLOT_DEMO_H

#include <functional>
#include <string>
#include <vector>
#include <iostream>
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <map>
#include <exception>
#include <array>

#ifdef DEBUG_SIGNALS
    #define SIGNAL_DEBUG(msg) std::cout << "[Signal Debug] " << msg << std::endl
#else
    #define SIGNAL_DEBUG(msg)
#endif

// 在文件开头添加前向声明
class EventQueue;
class EventFilter;

// 定义事件结构体
struct Event {
    std::function<void()> callback;
    enum Priority {
        HighPriority,
        NormalPriority,
        LowPriority
    } priority;
};

// 事件过滤器基类定义
class EventFilter {
public:
    virtual ~EventFilter() = default;
    virtual bool filterEvent(const Event& event) = 0;
};

// 线程安全的事件队列
class EventQueue {
public:
    using Priority = Event::Priority;
    using Event = ::Event;  // 使用全局的Event定义

    friend class EventFilter;  // 允许 EventFilter 访问 Event 结构体

    void postEvent(Event event, Priority priority = Priority::NormalPriority) {
        std::lock_guard<std::mutex> lock(this->mutex);
        event.priority = priority;
        if (event.priority == Priority::HighPriority) {
            highPriorityEvents.push(event);
        } else if (event.priority == Priority::LowPriority) {
            lowPriorityEvents.push(event);
        } else {
            normalPriorityEvents.push(event);
        }
        condition.notify_one();
    }

    // 添加唤醒方法
    void wakeUp() {
        condition.notify_all();  // 唤醒所有等待的线程
    }

    void processEvents() {
        std::unique_lock<std::mutex> lock(mutex);
        // 修改等待条件，增加退出检查
        condition.wait(lock, [this] { 
            return !highPriorityEvents.empty() || 
                   !normalPriorityEvents.empty() || 
                   !lowPriorityEvents.empty() || 
                   shouldExit;  // 增加退出标志检查
        });

        if (shouldExit) {
            return;
        }

        while (!highPriorityEvents.empty()) {
            Event event = highPriorityEvents.front();
            highPriorityEvents.pop();
            
            // 释放锁执行回调
            lock.unlock();
            event.callback();
            lock.lock();
        }

        while (!normalPriorityEvents.empty()) {
            Event event = normalPriorityEvents.front();
            normalPriorityEvents.pop();
            
            // 释放锁执行回调
            lock.unlock();
            event.callback();
            lock.lock();
        }

        while (!lowPriorityEvents.empty()) {
            Event event = lowPriorityEvents.front();
            lowPriorityEvents.pop();
            
            // 释放锁执行回调
            lock.unlock();
            event.callback();
            lock.lock();
        }
    }

    bool hasEvents() const {
        std::lock_guard<std::mutex> lock(this->mutex);
        return !highPriorityEvents.empty() || 
               !normalPriorityEvents.empty() || 
               !lowPriorityEvents.empty();
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(this->mutex);
        shouldExit = true;
        condition.notify_all();
    }

    bool isRunning() const {
        return !shouldExit;
    }

    void installEventFilter(EventFilter* filter) {
        eventFilters.push_back(filter);
    }

private:
    // 使用对象池来减少内存分配
    static constexpr size_t POOL_SIZE = 1000;
    std::array<Event, POOL_SIZE> eventPool;
    size_t poolIndex = 0;
    std::queue<Event> highPriorityEvents;
    std::queue<Event> normalPriorityEvents;
    std::queue<Event> lowPriorityEvents;
    mutable std::mutex mutex;  // 修改为 mutable，允许在 const 成员函数中修改
    std::condition_variable condition;
    bool shouldExit = false;  // 添加退出标志
    std::vector<EventFilter*> eventFilters;
};

// 模拟 QObject 的基类
class Object {
public:
    enum ConnectionType {
        DirectConnection,    // 直接调用
        QueuedConnection,   // 队列调用（跨线程）
        BlockingQueuedConnection,  // 阻塞队列调用
        AutoConnection     // 自动选择
    };

    // 用于存储信号和槽的连接
    struct Connection {
        void* sender;
        void* receiver;
        std::function<void()> slot;
        std::size_t signal_id;
        ConnectionType type;
    };

    static std::vector<Connection> connections;

    // 存储线程ID和事件队列的映射
    static std::map<std::thread::id, EventQueue*> threadQueues;
    static std::mutex queuesMutex;

    // 添加互斥锁保护连接列表
    static std::mutex connectionsMutex;

    // 注册线程的事件队列
    static void registerThreadQueue(EventQueue* queue) {
        std::lock_guard<std::mutex> lock(queuesMutex);
        threadQueues[std::this_thread::get_id()] = queue;
    }

    // 获取指定线程的事件队列
    static EventQueue* getThreadQueue(std::thread::id threadId) {
        std::lock_guard<std::mutex> lock(queuesMutex);
        auto it = threadQueues.find(threadId);
        return it != threadQueues.end() ? it->second : nullptr;
    }

    // 存储对象所属的线程ID
    std::thread::id threadId;

    Object() : threadId(std::this_thread::get_id()) {}

    // 获取当前线程的事件队列
    static EventQueue& getThreadEventQueue() {
        static thread_local EventQueue eventQueue;
        return eventQueue;
    }

    // 模拟 Qt 的 connect 函数
    template<typename Sender, typename Signal, typename Receiver, typename Slot>
    static void connect(Sender* sender, Signal signal, Receiver* receiver, Slot slot, 
                       ConnectionType type = DirectConnection) {
        std::lock_guard<std::mutex> lock(connectionsMutex);  // 保护连接操作
        Connection conn;
        conn.sender = sender;
        conn.receiver = receiver;
        conn.signal_id = reinterpret_cast<std::size_t>(&signal);
        conn.slot = [=]() { (receiver->*slot)(); };
        conn.type = type;
        connections.push_back(conn);
    }

    // 添加断开连接的功能
    static void disconnect(void* sender, void* receiver = nullptr) {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        auto it = std::remove_if(connections.begin(), connections.end(),
            [sender, receiver](const Connection& conn) {
                return conn.sender == sender && 
                       (!receiver || conn.receiver == receiver);
            });
        connections.erase(it, connections.end());
    }

    // 清理线程队列
    static void unregisterThreadQueue() {
        std::lock_guard<std::mutex> lock(queuesMutex);
        threadQueues.erase(std::this_thread::get_id());
    }

    // 析构函数
    virtual ~Object() {
        disconnect(this);  // 断开所有相关连接
    }

    void deleteLater() {
        EventQueue::Event event;
        event.callback = [this]() { delete this; };
        if (auto queue = getThreadQueue(threadId)) {
            queue->postEvent(event);
        }
    }

    void safeDelete() {
        disconnect(this);  // 断开所有连接
        if (std::this_thread::get_id() == threadId) {
            delete this;
        } else {
            deleteLater();
        }
    }

protected:
    template<typename Sender, typename Signal>
    void emit_signal(Sender* sender, Signal signal) {
        try {
            std::size_t signal_id = reinterpret_cast<std::size_t>(&signal);
            for (const auto& conn : connections) {
                if (conn.sender == sender && conn.signal_id == signal_id) {
                    if (conn.type == DirectConnection) {
                        try {
                            conn.slot();
                        } catch (const std::exception& e) {
                            std::cerr << "Error in slot execution: " << e.what() << std::endl;
                        }
                    } else {
                        auto receiver = static_cast<Object*>(conn.receiver);
                        if (auto queue = getThreadQueue(receiver->threadId)) {
                            EventQueue::Event event;
                            event.callback = conn.slot;
                            queue->postEvent(event, Event::Priority::NormalPriority);  // 修改这里
                        } else {
                            std::cerr << "No event queue found for receiver thread" << std::endl;
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in signal emission: " << e.what() << std::endl;
        }
    }

    void handleError(const std::string& message) {
        // 错误处理逻辑
    }
};

// 初始化静态成员
std::vector<Object::Connection> Object::connections;
std::map<std::thread::id, EventQueue*> Object::threadQueues;
std::mutex Object::queuesMutex;
std::mutex Object::connectionsMutex;

// 模拟一个按钮类
class Button : public Object {
public:
    Button(const std::string& text) : m_text(text) {}

    // 定义信号类型（使用函数指针）
    typedef void (Button::*SignalType)();
    
    // 声明信号
    SignalType clicked = nullptr;

    // 模拟点击事件
    void click() {
        std::cout << "Button \"" << m_text << "\" clicked\n";
        emit_signal(this, clicked);  // 发送特定信号
    }

private:
    std::string m_text;
};

// 模拟一个标签类
class Label : public Object {
public:
    Label(const std::string& text) : m_text(text) {}

    // 槽函数
    void updateText() {
        m_count++;
        std::cout << "Label text updated: " << m_text << " " << m_count << "\n";
    }

private:
    std::string m_text;
    int m_count = 0;
};

// 模拟一个工作线程类
class Worker : public Object {
public:
    Worker(const std::string& name) : m_name(name) {}

    typedef void (Worker::*SignalType)();
    SignalType workFinished = nullptr;

    void doWork() {
        std::cout << "Worker \"" << m_name << "\" starting work in thread " 
                  << std::this_thread::get_id() << "\n";
        
        // 模拟耗时操作
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        std::cout << "Worker \"" << m_name << "\" finished work\n";
        emit_signal(this, workFinished);
    }

private:
    std::string m_name;
};

// 模拟一个结果处理类
class ResultHandler : public Object {
public:
    ResultHandler(const std::string& name) : m_name(name) {}

    void handleResult() {
        std::cout << "ResultHandler \"" << m_name << "\" handling result in thread " 
                  << std::this_thread::get_id() << "\n";
    }

private:
    std::string m_name;
};

// 支持带参数的信号和槽
template<typename... Args>
struct Signal {
    typedef void (Object::*Type)(Args...);
};

template<typename Sender, typename... Args>
void emit_signal(Sender* sender, typename Signal<Args...>::Type signal, Args... args) {
    // ...
}

class SignalSlotException : public std::exception {
    // ...
};

template<typename Sender, typename Signal, typename Receiver, typename Slot>
static bool verifyConnection(Sender* sender, Signal signal, Receiver* receiver, Slot slot) {
    return sender && receiver && signal && slot;
}

#endif // SIGNAL_SLOT_DEMO_H 