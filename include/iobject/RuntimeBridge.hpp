#pragma once

#include "IRuntimeObject.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace iobject {

/// 远程对象句柄：取对象指针的数值形式，0 表示无效。同一对象在任何会话中都是同一数值；
/// 句柄必须先在本会话登记（经解析获得）才可用，未登记或已失效的数值不会被解析。
using RemoteObjectHandle = std::uint64_t;

/// 推送给远程端的事件消息。
/// 不传输通用事件载荷；仅当载荷可 As<DataChannelChangedEventData>() 时填充 channel。
/// channel 非空且事件源当次 ReadData 成功时，data 携带该通道的字节快照（含合法的空字节）；
/// 其余事件或读取失败时 data 不存在，远程端应回退到主动 ReadData 拉取。
struct RemoteEventMessage {
    RemoteObjectHandle source = 0;
    RuntimeEventType type;
    DataChannel channel;
    std::optional<std::vector<std::uint8_t>> data;
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
    /// 调用远程对象暴露的命名方法；handle 无效、method 为空或对象无此方法时返回 false。
    /// 成功时 result 恰好一次回调（空字节表示无返回值）；args/result 均为不透明字节。
    bool Invoke(RemoteObjectHandle handle, MethodView method, ByteInput args, DataReceiver result);
    /// 返回 handle 下所有直接子节点的 {name, handle} 快照；每个子节点自动登记句柄。
    /// handle 无效返回空列表。供远程发现（测试工具）使用，后期可能移除。
    std::vector<std::pair<std::string, RemoteObjectHandle>> GetChildren(RemoteObjectHandle handle);

    /// 对应 JS obj.SubscribeEvent(type, handler)；返回会话内订阅 ID，0 表示失败。
    /// 回调在 C++ 事件同步派发期间执行，不得向框架抛出异常。
    std::uint64_t SubscribeEvent(RemoteObjectHandle handle, RuntimeEventTypeView type,
                                 RemoteEventCallback callback);
    void CancelEvent(std::uint64_t subscriptionId) noexcept;

    /// 关闭会话：全部句柄与订阅立即失效；幂等。
    void Close() noexcept;
    bool IsOpen() const noexcept;

    /// 根锚点在会话内的句柄；会话打开期间有效，关闭后返回 0。
    RemoteObjectHandle RootObject() const noexcept;
    /// 句柄有效性查询：会话打开且句柄已登记时返回 true。
    bool HasObject(RemoteObjectHandle handle) const noexcept;

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
