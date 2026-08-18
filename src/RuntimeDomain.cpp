#include <iobject/RuntimeDomain.hpp>

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>

namespace iobject {

RuntimeDomain::RuntimeDomain()
    : rootAnchor_(Runtime::make()),
      bridgeRoot_(std::make_unique<RuntimeBridgeRoot>(rootAnchor_)) {}

RuntimeDomain::~RuntimeDomain() {
    bridgeRoot_.reset();  // 先停桥接入口。
    delete rootAnchor_;   // 再销毁根锚点（析构自动执行释放流程）。
}

IRuntimeObject* RuntimeDomain::RootAnchor() const noexcept {
    return rootAnchor_;
}

RuntimeBridgeRoot& RuntimeDomain::BridgeRoot() const noexcept {
    return *bridgeRoot_;
}

} // namespace iobject
