#include "TestCheck.hpp"

#include <iobject/Executor.hpp>
#include <iobject/Runtime.hpp>

#include <atomic>
#include <cstdio>
#include <thread>

int main() {
#ifdef NDEBUG
    std::printf("线程亲和断言在 Release（NDEBUG）下被编译掉，跳过负向检查\n");
    return 0;
#else
    std::atomic<bool> loopStarted{false};

    // 用 worker 线程跑 Run，使"循环线程"= worker；main 就不在循环线程。
    std::thread loopThread([&]() {
        iobject::Post([&]() {
            // 正向：循环线程上做操作，不应触发断言。
            iobject::IRuntimeObject* ok = iobject::Runtime::make();
            iobject::IRuntimeObject* child = iobject::Runtime::make();
            TEST_CHECK(ok->Connect("child", child));
            delete ok;
            delete child;
            loopStarted = true;
        });
        iobject::Run();
    });

    // 等循环线程就绪（Run 已把循环线程设为 worker）
    while (!loopStarted.load()) {
        std::this_thread::yield();
    }

    // 负向：main 线程（非循环线程）上做操作 → 触发断言 → abort
    iobject::IRuntimeObject* obj = iobject::Runtime::make();
    obj->Connect("child", iobject::Runtime::make());  // 应在此 abort

    // 不可达
    iobject::Stop();
    loopThread.join();
    delete obj;
    TEST_CHECK(false);   // 不应执行到这里
    return 0;
#endif
}
