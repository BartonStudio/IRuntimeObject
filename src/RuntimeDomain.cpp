#include <iobject/RuntimeDomain.hpp>

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>

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
    // 先撤桥接入口，再销毁根锚点（析构自动执行释放流程）。
    // 传输服务（如 WebSocketServer）由宿主在域析构前自行停止，见 WebSocketServer。
    bridgeRoot_.reset();
    rootAnchor_.reset();
}

IRuntimeObject* RuntimeDomain::RootAnchor() const noexcept {
    return rootAnchor_.get();
}

RuntimeBridgeRoot& RuntimeDomain::BridgeRoot() const noexcept {
    return *bridgeRoot_;
}

} // namespace iobject
