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

    /// domain 是本连接对应的域名；Connect 请求的 domain 不匹配时握手失败并关闭连接。
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

} // namespace iobject
