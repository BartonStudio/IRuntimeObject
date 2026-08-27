// 12_ExecutorTest：演示环境式事件循环（默认单线程线程模型）+ 线程亲和。
// 任何线程用 iobject::Post() 投递任务；main 用 iobject::Run() 阻塞跑循环；
// IRuntimeObject 业务在循环线程上执行，Debug 下非循环线程操作会触发亲和断言。
#include <iobject/Executor.hpp>
#include <iobject/Runtime.hpp>

#include <cstdio>
#include <thread>

namespace {

// 业务对象：计数器，暴露 Invoke 命令（属于线程亲和断言守护的 IRuntimeObject 业务）。
class Counter {
public:
    bool Invoke(iobject::MethodView method, iobject::ByteInput args, iobject::DataReceiver result) {
        static_cast<void>(args);
        if (method == "Increment") {
            ++count_;
            result(iobject::ByteView{});
            return true;
        }
        return false;
    }
    int count() const { return count_; }

private:
    int count_ = 0;
};

} // namespace

int main() {
    // ① 装配业务对象（Run 之前，循环线程未激活，主线程直接操作即可）
    iobject::IRuntimeObject* counter = iobject::Runtime::make<Counter>();
    std::printf("1. 装配业务对象 Counter\n");

    // ② 循环线程上的 IRuntimeObject 业务：Invoke 命令（亲和断言放行）
    iobject::Post([counter]() {
        std::printf("4. 循环线程 Invoke(\"Increment\")：IsOnLoopThread=%d\n",
                    iobject::IsOnLoopThread() ? 1 : 0);
        counter->Invoke("Increment", {}, [](iobject::ByteView) {});
        std::printf("   Counter 计数 = %d\n", counter->As<Counter>()->count());
    });

    // ③ worker 线程投递任务（跨线程 Post 落回循环线程）
    std::thread worker([]() {
        std::printf("2. worker 线程：IsOnLoopThread=%d（不在循环线程）\n",
                    iobject::IsOnLoopThread() ? 1 : 0);
        iobject::Post([]() {
            std::printf("5. worker 投递的任务在循环线程执行，随后 Stop\n");
            iobject::Stop();
        });
    });
    worker.join();

    std::printf("3. main 调用 iobject::Run() 阻塞跑循环\n");
    iobject::Run();
    std::printf("6. Run 返回（已 Stop）\n");

    std::printf("7. 说明：循环运行期间 IRuntimeObject 操作必须在循环线程，\n"
                "   非循环线程操作在 Debug 下会触发线程亲和断言\n"
                "   （负向演示见 tests/AffinityAssertTest.cpp）\n");

    delete counter;
    return 0;
}
