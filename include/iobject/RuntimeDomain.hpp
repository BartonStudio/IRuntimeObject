#pragma once

#include <memory>

namespace iobject {

class IRuntimeObject;
class RuntimeBridgeRoot;

/// 运行时域：第一版对应进程内默认全局拓扑，自动创建并持有唯一根锚点与桥接入口。
/// 一个活动 IRuntimeObject 同一时刻有且仅属于一个域，生命周期内不迁移。
/// 硬约束（约定，不加运行时分支）：域及桥接服务存活期间，不得对根锚点 Release 或 delete。
/// 正常销毁顺序：先关闭全部 RuntimeSession，再销毁业务对象，最后销毁 RuntimeDomain。
class RuntimeDomain final {
public:
    RuntimeDomain();
    ~RuntimeDomain();

    RuntimeDomain(const RuntimeDomain&) = delete;
    RuntimeDomain& operator=(const RuntimeDomain&) = delete;

    /// 域持有的纯运行时根锚点；业务方用 Connect 把业务对象接入其子树。
    IRuntimeObject* RootAnchor() const noexcept;
    /// 域内唯一桥接入口；所有远程会话经它创建。
    RuntimeBridgeRoot& BridgeRoot() const noexcept;

private:
    // 声明顺序是正确性依赖：bridgeRoot_ 必须先于根锚点销毁，不得调换。
    std::unique_ptr<IRuntimeObject> rootAnchor_;
    std::unique_ptr<RuntimeBridgeRoot> bridgeRoot_;
};

} // namespace iobject
