#pragma once

#include "IRuntimeObject.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace iobject {

/// 会话内不透明远程对象句柄；0 表示无效。不暴露内存地址，跨会话不可混用。
using RemoteObjectHandle = std::uint64_t;

/// 推送给远程端的事件消息。
/// 第一版不传输通用事件载荷；仅当载荷可 As<DataChannelChangedEventData>() 时填充 channel。
struct RemoteEventMessage {
    RemoteObjectHandle source = 0;
    RuntimeEventType type;
    DataChannel channel;
};

using RemoteEventCallback = std::function<void(const RemoteEventMessage&)>;

/// 一个远程连接对应一个会话；方法逐一对应 JS 端接口，未来传输层把协议消息转发到这里。
/// 会话不是线程安全的：与框架其余部分一样假定单线程事件循环。
class RuntimeSession final {
public:
    ~RuntimeSession();

    RuntimeSession(const RuntimeSession&) = delete;
    RuntimeSession& operator=(const RuntimeSession&) = delete;

    /// 对应 JS runtime.Root.GetChildItem(path)；未命中返回 0。
    RemoteObjectHandle ResolveRootChild(const std::string& path);
    /// 对应 JS obj.GetChildItem(path)；handle 无效或路径未命中返回 0。
    RemoteObjectHandle ResolveChild(RemoteObjectHandle handle, const std::string& path);

    bool ReadData(RemoteObjectHandle handle, DataChannelView channel, DataReceiver receiver) const;
    bool WriteData(RemoteObjectHandle handle, DataChannelView channel, ByteInput data);

    /// 对应 JS obj.SubscribeEvent(type, handler)；返回会话内订阅 ID，0 表示失败。
    /// 回调在 C++ 事件同步派发期间执行，不得向框架抛出异常。
    std::uint64_t SubscribeEvent(RemoteObjectHandle handle, RuntimeEventTypeView type,
                                 RemoteEventCallback callback);
    void CancelEvent(std::uint64_t subscriptionId) noexcept;

    /// 关闭会话：全部句柄与订阅立即失效；幂等。
    void Close() noexcept;
    bool IsOpen() const noexcept;

private:
    friend class RuntimeBridgeRoot;
    RuntimeSession(IRuntimeObject* rootAnchor, IRuntimeObject* relay);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 域内唯一桥接入口；不拥有根锚点（根锚点由 RuntimeDomain 持有）。
/// 只能由 RuntimeDomain 构造，保证域内唯一。
class RuntimeBridgeRoot final {
public:
    RuntimeBridgeRoot(const RuntimeBridgeRoot&) = delete;
    RuntimeBridgeRoot& operator=(const RuntimeBridgeRoot&) = delete;

    /// 根锚点不可用时返回 nullptr。会话由调用方持有，须先于 RuntimeDomain 销毁。
    std::unique_ptr<RuntimeSession> OpenSession();

private:
    friend class RuntimeDomain;
    explicit RuntimeBridgeRoot(IRuntimeObject* rootAnchor) noexcept : rootAnchor_(rootAnchor) {}

    IRuntimeObject* rootAnchor_;
};

} // namespace iobject
