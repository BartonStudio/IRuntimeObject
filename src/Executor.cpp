#include <iobject/Executor.hpp>

#include "ThreadAffinity.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace iobject {

struct SingleThreadExecutor::Impl {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> queue;
    std::atomic<bool> stopped{false};
    std::thread::id owner;
};

SingleThreadExecutor::SingleThreadExecutor() : impl_(std::make_unique<Impl>()) {}

SingleThreadExecutor::~SingleThreadExecutor() {
    Stop();
}

void SingleThreadExecutor::Post(std::function<void()> task) {
    if (!task) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->queue.push_back(std::move(task));
    }
    impl_->ready.notify_one();
}

void SingleThreadExecutor::Run() {
    detail::setLoopThread(std::this_thread::get_id());
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->owner = std::this_thread::get_id();
    }
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(impl_->mutex);
            impl_->ready.wait(lock, [this]() {
                return impl_->stopped.load(std::memory_order_relaxed) || !impl_->queue.empty();
            });
            if (impl_->stopped.load(std::memory_order_relaxed)) {
                break;  // 停止：丢弃剩余排队任务。
            }
            task = std::move(impl_->queue.front());
            impl_->queue.pop_front();
        }
        task();  // 锁外执行，避免长任务持锁。
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->owner = std::thread::id();
    }
    detail::setLoopThread(std::thread::id());
}

void SingleThreadExecutor::Stop() noexcept {
    impl_->stopped.store(true, std::memory_order_relaxed);
    impl_->ready.notify_all();
}

bool SingleThreadExecutor::IsOnExecutionThread() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->owner != std::thread::id()
        && impl_->owner == std::this_thread::get_id();
}

struct HostLoopExecutor::Impl {
    HostLoopExecutor::RunCallback onRun;
    HostLoopExecutor::PostCallback onPost;
    HostLoopExecutor::StopCallback onStop;
    HostLoopExecutor::IsLoopThreadCallback isOnLoopThread;

    Impl(HostLoopExecutor::RunCallback r, HostLoopExecutor::PostCallback p,
         HostLoopExecutor::StopCallback s, HostLoopExecutor::IsLoopThreadCallback i)
        : onRun(std::move(r)), onPost(std::move(p)),
          onStop(std::move(s)), isOnLoopThread(std::move(i)) {}
};

HostLoopExecutor::HostLoopExecutor(RunCallback onRun, PostCallback onPost,
                                   StopCallback onStop, IsLoopThreadCallback isOnLoopThread)
    : impl_(std::make_unique<Impl>(std::move(onRun), std::move(onPost),
                                   std::move(onStop), std::move(isOnLoopThread))) {}

HostLoopExecutor::~HostLoopExecutor() = default;

void HostLoopExecutor::Post(std::function<void()> task) {
    if (!task) return;
    if (impl_->onPost) impl_->onPost(std::move(task));
}

void HostLoopExecutor::Run() {
    detail::setLoopThread(std::this_thread::get_id());
    if (impl_->onRun) impl_->onRun();
    detail::setLoopThread(std::thread::id());
}

void HostLoopExecutor::Stop() noexcept {
    if (impl_->onStop) impl_->onStop();
}

bool HostLoopExecutor::IsOnExecutionThread() const {
    return impl_->isOnLoopThread && impl_->isOnLoopThread();
}

// —— 环境式入口 ——

namespace detail {

// 与 runtimeTopology() 一致：堆分配、永不析构，规避静态析构顺序问题。
std::unique_ptr<Executor>& globalExecutor() {
    static auto* executor = new std::unique_ptr<Executor>(
        std::make_unique<SingleThreadExecutor>());
    return *executor;
}

} // namespace detail

void Run() {
    detail::globalExecutor()->Run();
}

void Post(std::function<void()> task) {
    detail::globalExecutor()->Post(std::move(task));
}

void Stop() noexcept {
    detail::globalExecutor()->Stop();
}

bool IsOnLoopThread() {
    return detail::globalExecutor()->IsOnExecutionThread();
}

void UseExecutor(std::unique_ptr<Executor> executor) {
    if (executor) {
        detail::globalExecutor() = std::move(executor);
    }
}

} // namespace iobject
