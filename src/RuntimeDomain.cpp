#include <iobject/RuntimeDomain.hpp>

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>

#include <utility>

namespace iobject {

RuntimeDomain::RuntimeDomain()
    : rootAnchor_(Runtime::make()),
      // RuntimeBridgeRoot 构造为私有，friend 关系不传递到 make_unique，故用直接 new 包裹；
      // unique_ptr 裸指针构造为 noexcept，若此处 new 抛异常 rootAnchor_ 仍会自动回收。
      bridgeRoot_(std::unique_ptr<RuntimeBridgeRoot>(new RuntimeBridgeRoot(rootAnchor_.get()))) {}

RuntimeDomain::RuntimeDomain(IRuntimeObject* root)
    : rootAnchor_(root),
      bridgeRoot_(std::unique_ptr<RuntimeBridgeRoot>(new RuntimeBridgeRoot(rootAnchor_.get()))) {}

RuntimeDomain::~RuntimeDomain() {
    // 成员声明序已保证按 webSocketServer_ → bridgeRoot_ → rootAnchor_ 析构，
    // 这里显式写清三步语义：先停内置 ws 服务（关闭全部远程会话），
    // 再撤桥接入口，最后销毁根锚点（析构自动执行释放流程）。
    webSocketServer_.reset();
    bridgeRoot_.reset();
    rootAnchor_.reset();
}

IRuntimeObject* RuntimeDomain::RootAnchor() const noexcept {
    return rootAnchor_.get();
}

RuntimeBridgeRoot& RuntimeDomain::BridgeRoot() const noexcept {
    return *bridgeRoot_;
}

void RuntimeDomain::startBuiltinWebSocketServer(WebSocketServer::Config config) {
    // 可选内置远程能力：由业务方显式调用，把 ws 服务端挂到根节点 "WebSocket" 下。
    // WebSocketServer 构造即监听；端口冲突等启动失败仅记日志（详见类注释）。
    webSocketServer_.reset(
        Runtime::make<WebSocketServer>(*bridgeRoot_, std::move(config)));
    rootAnchor_->Connect("WebSocket", webSocketServer_.get());
}

} // namespace iobject
