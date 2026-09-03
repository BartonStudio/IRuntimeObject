#pragma once

#include <memory>

namespace iobject {

class IRuntimeObject;
class RuntimeBridgeRoot;

/// 运行时域：第一版对应进程内默认全局拓扑，自动创建并持有唯一根锚点与桥接入口。
/// 一个活动 IRuntimeObject 同一时刻有且仅属于一个域，生命周期内不迁移。
/// 第一版每个进程只应存在一个 RuntimeDomain 实例；多个实例会共享同一全局拓扑，
/// 跨"域"关系不会被阻止。
/// 硬约束（约定，不加运行时分支）：域及桥接服务存活期间，不得对根锚点 Release 或 delete。
/// 正常销毁顺序：先关闭全部 RuntimeSession，再销毁业务对象，最后销毁 RuntimeDomain。
///
/// 内置能力：构造时自动启动 WebSocket 远程服务端（websocketpp），以 "WebSocket"
/// 子节点挂在根锚点下（只读通道 "Port"），默认监听 9002、握手域名 "iobject"，
/// 应用侧无需任何代码即可通过 ws://127.0.0.1:9002 以 IObject 远程协议访问整棵对象树。
/// 端口被占用时启动失败仅记日志，不影响域本身。
class RuntimeDomain final {
public:
    /// 默认构造：根锚点为空的纯运行时节点。
    RuntimeDomain();
    /// 以自定义根节点构造：root 由本域接管所有权（析构时 delete）。
    explicit RuntimeDomain(IRuntimeObject* root);
    ~RuntimeDomain();

    RuntimeDomain(const RuntimeDomain&) = delete;
    RuntimeDomain& operator=(const RuntimeDomain&) = delete;

    /// 域持有的纯运行时根锚点；业务方用 Connect 把业务对象接入其子树。
    IRuntimeObject* RootAnchor() const noexcept;
    /// 域内唯一桥接入口；所有远程会话经它创建。
    RuntimeBridgeRoot& BridgeRoot() const noexcept;

private:
    void startBuiltinWebSocketServer();

    // 声明顺序是正确性依赖，析构顺序与之相反：
    //   webSocketServer_（停 ws 服务并关闭全部远程会话）
    //   → bridgeRoot_（撤桥接入口）→ rootAnchor_（销毁根锚点）。不得调换。
    std::unique_ptr<IRuntimeObject> rootAnchor_;
    std::unique_ptr<RuntimeBridgeRoot> bridgeRoot_;
    std::unique_ptr<IRuntimeObject> webSocketServer_;
};

} // namespace iobject
