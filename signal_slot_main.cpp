#include "signal_slot_demo.h"
#include <thread>
#include <atomic>
#include <iostream>

void processEvents(EventQueue* queue) {
    while (true) {
        queue->processEvents();
    }
}

int main() {
    // 创建主线程的事件队列
    EventQueue mainQueue;
    Object::registerThreadQueue(&mainQueue);

    // 创建工作线程对象和结果处理器
    Worker worker("WorkerThread");
    ResultHandler handler("MainThread");
    handler.threadId = std::this_thread::get_id();  // 确保处理器在主线程

    // 使用队列连接方式（跨线程）
    Object::connect(&worker, &Worker::workFinished, &handler, &ResultHandler::handleResult, 
                   Object::QueuedConnection);

    std::atomic<bool> running{true};
    
    // 修改事件处理循环
    std::thread eventThread([&mainQueue, &running]() {
        std::cout << "Event thread started: " << std::this_thread::get_id() << std::endl;
        while (running) {
            try {
                mainQueue.processEvents();
            } catch (const std::exception& e) {
                std::cerr << "Error in event processing: " << e.what() << std::endl;
            }
            if (!running) break;
        }
        Object::unregisterThreadQueue();
    });

    // 创建工作线程的事件队列
    EventQueue workerQueue;
    
    // 启动工作线程
    std::thread workerThread([&worker, &workerQueue]() {
        Object::registerThreadQueue(&workerQueue);
        worker.threadId = std::this_thread::get_id();
        std::cout << "Worker thread started: " << std::this_thread::get_id() << std::endl;
        try {
            worker.doWork();
        } catch (const std::exception& e) {
            std::cerr << "Error in worker thread: " << e.what() << std::endl;
        }
    });

    // 等待工作线程完成
    workerThread.join();
    std::cout << "Worker thread finished" << std::endl;

    // 处理所有待处理的事件
    while (mainQueue.hasEvents()) {
        mainQueue.processEvents();
    }

    // 优雅退出
    running = false;
    mainQueue.shutdown();  // 使用新的shutdown方法
    eventThread.join();
    std::cout << "Event thread finished" << std::endl;

    return 0;
} 