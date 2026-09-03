#pragma once

#include "RuntimeBridgeProtocol.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace iobject {

class RuntimeBridgeRoot;

/// IObject 内置 WebSocket 远程服务端（传输载体：websocketpp + standalone Asio）。
///
/// 定位：RuntimeDomain 构造时自动创建、Start 并挂到根节点 "WebSocket" 下，
/// 应用侧无需任何代码即可通过 ws://host:port + IObject 远程协议（MessagePack）
/// 访问整棵对象树；协议与 webview 链路完全一致，双工双向。
///
/// 线程模型：websocketpp 由独立后台线程驱动 io_service；所有触碰 IObject
/// 对象树的操作（RuntimeBridgePeer::ReceiveMessage / 析构）一律经 iobject::Post
/// 封回事件循环线程串行执行，保证框架单线程约定不被破坏。
///
/// 数据通道（远程可读）：
///   - "Port"：2 字节大端 uint16，监听端口
///
/// 容错：Start 失败（如端口被占用）不抛异常，仅记录日志并进入未运行状态，
/// 不影响宿主进程；IsRunning() 可查询。
class WebSocketServer {
public:
    /// 内置服务端配置。
    struct Config {
        /// 监听端口；默认 9002。
        std::uint16_t port = 9002;
        /// 握手校验的域名字符串；客户端 Connect 请求的 domain 必须与之一致。
        /// 默认 "iobject"，与 webview 链路（各持独立 domain）互不相关。
        std::string domain = "iobject";
    };

    /// 构造即启动：端口监听与后台事件线程在构造函数内拉起。
    /// Start 失败不抛异常（见类注释"容错"）。
    explicit WebSocketServer(RuntimeBridgeRoot& bridgeRoot, Config config = Config{});
    /// 析构即停止：关闭全部连接与远程会话、回收后台线程。幂等。
    ~WebSocketServer();

    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    /// 端口监听是否成功建立（含后台线程存活）。
    bool IsRunning() const noexcept;

    /// 本服务端配置的监听端口。
    std::uint16_t Port() const noexcept;

    /// 本服务端配置的握手域名。
    const std::string& Domain() const noexcept;

    /// 停止服务：关闭全部连接、销毁全部远程会话、回收后台线程。幂等。
    /// 建议在 iobject::Run() 返回之后调用（RuntimeDomain 析构天然满足）；
    /// 循环线程仍在运行时也能安全调用（会话清理会 Post 回循环线程）。
    void Stop() noexcept;

    /// —— IObject 原生对象契约（供 Runtime::make 包装为节点）——
    /// 标记：通道读取线程安全（端口为构造后不变量）。
    static constexpr bool kThreadSafe = true;

    /// 只读通道 "Port"：2 字节大端 uint16。
    bool ReadData(DataChannelView channel, DataReceiver receiver) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iobject
