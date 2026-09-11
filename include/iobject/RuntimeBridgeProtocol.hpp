#pragma once

#include "IRuntimeObject.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace iobject {

class RuntimeBridgeRoot;

/// MessagePack 协议适配器：把一个传输连接映射到一个 RuntimeSession。
/// 传输层每收到一条完整消息就调用 ReceiveMessage；适配器经 SendCallback 发回响应帧与事件帧。
/// 帧字节仅在 SendCallback 调用期间有效，传输层需要保留时必须自行复制。
/// 非线程安全，与框架其余部分一样假定单线程事件循环。
class RuntimeBridgePeer final {
public:
    /// 传输层发送一帧（完整 MessagePack 文档）给客户端的回调。
    using SendCallback = std::function<void(ByteView frame)>;

    /// domain：本连接对应的「域路由键」。
    ///
    /// 【设计本意】domain 不是鉴权口令、也不是服务标识——传输端点（如 ws://host:port）
    /// 本身已经唯一确定了「哪个服务」。它用于「一个传输服务多个 RuntimeDomain（多棵对象树）」：
    /// 客户端在 Connect 请求里用 domain 声明「我要连哪棵树」，服务端据此路由，并把会话
    /// 挂到对应树的桥接入口上。
    ///
    /// 【隔离】会话由「它 Connect 的那个 domain 的 bridgeRoot」开出来，句柄与对象树只属于
    /// 那棵树；不同 domain 的对象互不可见，B 域里找不到 A 域的对象。
    /// 单域应用可把它当作不透明常量；多域应用靠它把连接路由到正确的树。
    RuntimeBridgePeer(RuntimeBridgeRoot& bridgeRoot, std::string domain, SendCallback send);
    ~RuntimeBridgePeer();

    RuntimeBridgePeer(const RuntimeBridgePeer&) = delete;
    RuntimeBridgePeer& operator=(const RuntimeBridgePeer&) = delete;

    /// 传输层收到一条完整消息时调用；畸形消息回 MalformedMessage，超长消息回错并关闭连接。
    void ReceiveMessage(ByteView message);
    /// 关闭连接：关闭底层会话，之后不再发送任何帧；幂等。传输断开时必须调用。
    void Close() noexcept;
    bool IsOpen() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 供传输层在握手前做路由：从首条消息里提取 Connect 请求的 domain 字段。
/// 消息不是合法 MessagePack map、op 不是 "Connect"、或缺 domain 时返回空字符串。
/// 只解析、不建立会话、不触碰对象树，可在任意线程调用。
std::string PeekConnectDomain(ByteView message);

} // namespace iobject
