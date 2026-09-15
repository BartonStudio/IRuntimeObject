#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace iobject {

class RuntimeDomain;

/// IObject 内置 WebSocket 远程传输服务端（传输载体：websocketpp + standalone Asio）。
///
/// 【定位】它是「系统级传输服务」，不是域（RuntimeDomain）的成员，也不是对象树里的节点。
/// 一个进程通常只建一个实例；一个实例可服务多个域（多棵对象树）：经 BindDomain 注册
/// 「域名字 → 对象树入口」的路由表，客户端 Connect{domain} 时据此把会话挂到对应树。
/// 域的名字取自 RuntimeDomain::Name()，一棵树一个名字，1:1，无别名。
///
/// 【隔离】会话由「它 Connect 的那个 domain 的 bridgeRoot」开出来，句柄与对象树只属于
/// 那棵树；不同 domain 的对象互不可见，B 域里找不到 A 域的对象。
///
/// 【domain 的含义】见 RuntimeBridgeProtocol.hpp：domain 是「域路由键」，不是鉴权口令/服务标识。
///
/// 【线程模型】websocketpp 由独立后台线程驱动 io_service；所有触碰 IObject 对象树的
/// 操作（RuntimeBridgePeer::ReceiveMessage / 析构 / 首帧路由后的建会话）一律经 iobject::Post
/// 封回事件循环线程串行执行，保证框架单线程约定不被破坏。
///
/// 【容错】Start 失败（如端口被占用）不抛异常，仅记录日志并进入未运行状态；
/// IsRunning() 可查询。生命周期建议与宿主进程一致。
class WebSocketServer {
public:
    /// 内置服务端配置。
    struct Config {
        /// 监听端口；默认 9002。
        std::uint16_t port = 9002;
    };

    /// 构造即启动：端口监听与后台事件线程在构造函数内拉起。
    /// Start 失败不抛异常（见类注释"容错"）。
    explicit WebSocketServer(Config config = Config{});
    /// 析构即停止：关闭全部连接与远程会话、回收后台线程。幂等。
    ~WebSocketServer();

    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    /// 注册一个域到路由表：以 domain.Name() 作为路由键，指向 domain.BridgeRoot()。
    /// 客户端 Connect{domain} 时按它路由；未注册的名字在握手时被拒绝（DomainNotFound 并断开）。
    /// 未命名（Name() 为空）的域会被忽略。同名覆盖旧映射。线程安全，可在监听启动前后调用。
    void BindDomain(RuntimeDomain& domain);

    /// 端口监听是否成功建立（含后台线程存活）。
    bool IsRunning() const noexcept;
    /// 本服务端配置的监听端口。
    std::uint16_t Port() const noexcept;
    /// 停止服务：关闭全部连接、销毁全部远程会话、回收后台线程。幂等。
    void Stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iobject
