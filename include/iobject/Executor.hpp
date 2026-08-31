#pragma once

#include <functional>
#include <memory>

namespace iobject {

/// 线程模型的统一抽象：保证已投递任务"串行执行"（一次一个、FIFO），
/// 从而保护框架核心（拓扑/订阅/通道）不受并发访问。
/// 业务代码只依赖此抽象与下面的环境式入口函数，不依赖具体线程模型。
class Executor {
public:
    virtual ~Executor() = default;

    /// 任意线程：投递任务，按 FIFO 串行执行。
    virtual void Post(std::function<void()> task) = 0;

    /// 阻塞当前线程运行事件循环，直到 Stop()；循环线程 = 调用 Run() 的线程。
    virtual void Run() = 0;

    /// 请求停止：Run() 在执行完当前任务后返回，丢弃剩余排队任务。
    virtual void Stop() noexcept = 0;

    /// 当前线程是否就是循环线程（调用 Run() 的线程）。
    virtual bool IsOnExecutionThread() const = 0;

    /// 非阻塞执行已排队任务（宿主自有循环集成用）；默认空实现，返回是否执行了任务。
    virtual bool Drain() { return false; }
};

/// 默认线程模型：单线程事件循环。Run() 阻塞调用线程；空闲时睡眠等待，不空转。
class SingleThreadExecutor final : public Executor {
public:
    SingleThreadExecutor();
    ~SingleThreadExecutor() override;

    SingleThreadExecutor(const SingleThreadExecutor&) = delete;
    SingleThreadExecutor& operator=(const SingleThreadExecutor&) = delete;

    void Post(std::function<void()> task) override;
    void Run() override;
    void Stop() noexcept override;
    bool IsOnExecutionThread() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 宿主循环集成执行器：把 IObject 任务队列挂到宿主自有的消息泵上
/// （例如 WebView2 / GLFW 等自带事件循环的宿主）。
/// 宿主只需提供四个回调；Run() 内部会正确登记/清空循环线程，
/// 保证 assertLoopThread() / IsOnLoopThread() 继续生效。
class HostLoopExecutor final : public Executor {
public:
    using RunCallback = std::function<void()>;
    using PostCallback = std::function<void(std::function<void()>)>;
    using StopCallback = std::function<void()>;
    using IsLoopThreadCallback = std::function<bool()>;

    HostLoopExecutor(RunCallback onRun, PostCallback onPost,
                     StopCallback onStop, IsLoopThreadCallback isOnLoopThread);
    ~HostLoopExecutor() override;

    HostLoopExecutor(const HostLoopExecutor&) = delete;
    HostLoopExecutor& operator=(const HostLoopExecutor&) = delete;

    void Post(std::function<void()> task) override;
    void Run() override;
    void Stop() noexcept override;
    bool IsOnExecutionThread() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// —— 环境式事件循环入口 ——
/// 委托给当前选中的执行器（默认懒初始化 SingleThreadExecutor）。
/// 业务与外部类只认这几个自由函数，不认具体线程模型。

/// 阻塞当前线程运行事件循环，直到 Stop()。
void Run();

/// 任意线程投递任务，循环线程串行执行。
void Post(std::function<void()> task);

/// 请求停止事件循环。
void Stop() noexcept;

/// 当前线程是否就是循环线程。
bool IsOnLoopThread();

/// 更换线程模型（A/B 测试入口）；必须在 Run() 之前调用，运行中调用是未定义行为。
void UseExecutor(std::unique_ptr<Executor> executor);

} // namespace iobject
