#pragma once

#include <atomic>
#include <thread>

namespace iobject::detail {

/// 循环线程亲和寄存器：空 id 表示"未运行循环"（断言放行）。
/// 仅供框架内部（Executor 与 RuntimeObject）使用，不对外安装。
inline std::atomic<std::thread::id>& loopThreadIdStorage() {
    static std::atomic<std::thread::id> id;
    return id;
}

/// 循环线程进入/退出：由 Executor 在 Run 开始/结束时调用。
inline void setLoopThread(std::thread::id id) noexcept {
    loopThreadIdStorage().store(id);
}

/// 当前循环线程（空 id = 未运行循环）。
inline std::thread::id loopThread() noexcept {
    return loopThreadIdStorage().load();
}

} // namespace iobject::detail
