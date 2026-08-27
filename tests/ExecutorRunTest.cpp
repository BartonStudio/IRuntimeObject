#include "TestCheck.hpp"

#include <iobject/Executor.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

int main() {
    std::vector<int> order;                  // 循环线程写、Run 后 main 读（同线程，安全）
    std::atomic<bool> taskOnLoop{false};     // 任务是否在循环线程执行
    std::atomic<bool> workerOffLoop{false};  // worker 是否不在循环线程

    // ① 任务 1：FIFO 起点 + 验证在循环线程执行
    iobject::Post([&]() {
        taskOnLoop = iobject::IsOnLoopThread();
        order.push_back(1);
    });

    // ② worker 线程投递任务 2，并同步保证它排在任务 3/4 之前
    std::mutex syncMutex;
    std::condition_variable syncCv;
    bool workerPosted = false;
    std::thread worker([&]() {
        workerOffLoop = !iobject::IsOnLoopThread();
        iobject::Post([&]() { order.push_back(2); });
        {
            std::lock_guard<std::mutex> lock(syncMutex);
            workerPosted = true;
        }
        syncCv.notify_one();
    });

    {
        std::unique_lock<std::mutex> lock(syncMutex);
        syncCv.wait(lock, [&] { return workerPosted; });
    }

    // ③ 任务 3 停止；④ 任务 4 应被丢弃
    iobject::Post([&]() {
        order.push_back(3);
        iobject::Stop();
    });
    iobject::Post([&]() { order.push_back(4); });

    worker.join();

    // main 调用 Run：阻塞执行队列，直到 Stop
    iobject::Run();

    TEST_CHECK(workerOffLoop.load());   // worker 不在循环线程
    TEST_CHECK(taskOnLoop.load());      // 任务在循环线程执行
    TEST_CHECK(order.size() == 3);      // 任务 4 被丢弃
    TEST_CHECK(order[0] == 1 && order[1] == 2 && order[2] == 3);  // FIFO
    return 0;
}
