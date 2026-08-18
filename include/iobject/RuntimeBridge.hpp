#pragma once

namespace iobject {

class IRuntimeObject;

/// 域内唯一桥接入口；所有远程会话经它创建。完整定义见后续任务。
class RuntimeBridgeRoot final {
public:
    explicit RuntimeBridgeRoot(IRuntimeObject* rootAnchor) noexcept : rootAnchor_(rootAnchor) {}

    RuntimeBridgeRoot(const RuntimeBridgeRoot&) = delete;
    RuntimeBridgeRoot& operator=(const RuntimeBridgeRoot&) = delete;

private:
    IRuntimeObject* rootAnchor_;
};

} // namespace iobject
