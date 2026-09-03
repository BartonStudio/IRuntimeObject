#include <iobject/WebSocketServer.hpp>

#include <iobject/Executor.hpp>
#include <iobject/Logger.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeBridgeProtocol.hpp>

// —— websocketpp / standalone Asio 仅在本实现文件内出现，公共头保持零依赖 ——

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif

// ASIO_STANDALONE / WEBSOCKETPP_STANDALONE 由构建系统（CMake）统一定义，
// 此处不再重复 #define，避免宏重定义告警。
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <asio.hpp>

#include <array>
#include <atomic>
#include <exception>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace iobject {

namespace {

using WsServer = websocketpp::server<websocketpp::config::asio>;

/// 每个客户端连接一份：连接句柄 + 对应的协议端点。
/// peer 的全部触碰（ReceiveMessage / 析构）都只发生在事件循环线程；
/// con 的 send/close 由 websocketpp 保证线程安全，可在任意线程调用。
struct ConnState {
    WsServer::connection_ptr con;                 // 强引用，保连接对象存活。
    std::unique_ptr<RuntimeBridgePeer> peer;     // 仅事件循环线程访问/销毁。
};

} // namespace

// =============================================================================
// Impl
// =============================================================================

struct WebSocketServer::Impl {
    RuntimeBridgeRoot* bridgeRoot = nullptr;  // 非拥有，由 RuntimeDomain 持有。
    WebSocketServer::Config config;

    asio::io_service io;
    WsServer server;
    Logger log;

    std::thread thread;          // websocketpp 事件线程。
    std::atomic<bool> running{false};

    std::mutex connectionsMutex;
    std::map<void*, std::shared_ptr<ConnState>> connections;  // 键 = hdl.lock().get()

    // —— wspp 线程回调（io.run() 所在线程）——

    void onOpen(websocketpp::connection_hdl hdl) {
        auto state = std::make_shared<ConnState>();
        state->con = server.get_con_from_hdl(hdl);
        // peer 在此处构造是安全的：构造只登记回调，不触碰对象树；
        // 真正的会话建立发生在协议层 Connect 握手（循环线程内）。
        state->peer = std::make_unique<RuntimeBridgePeer>(
            *bridgeRoot, config.domain,
            [state](ByteView frame) {
                // SendCallback 在事件循环线程被调用；connection::send 线程安全。
                // 错误码以返回值交付；失败通常意味着连接已在关闭流程中，交由 on_close 收尾。
                const std::string payload(reinterpret_cast<const char*>(frame.data()),
                                           frame.size());
                (void)state->con->send(payload, websocketpp::frame::opcode::binary);
            });
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            connections.emplace(hdl.lock().get(), std::move(state));
        }
        log.Log(LogLevel::Debug, "WebSocket", "客户端接入");
    }

    void onClose(websocketpp::connection_hdl hdl) {
        std::shared_ptr<ConnState> state;
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            const auto found = connections.find(hdl.lock().get());
            if (found == connections.end()) {
                return;
            }
            state = std::move(found->second);
            connections.erase(found);
        }
        if (!state) {
            return;
        }
        // peer 只能在事件循环线程销毁（其内部会触碰对象树）。
        // 队列 FIFO 保证此前入队的 ReceiveMessage 任务先于本任务执行。
        iobject::Post([state]() { state->peer.reset(); });
        log.Log(LogLevel::Debug, "WebSocket", "客户端断开，会话已关闭");
    }

    void onMessage(websocketpp::connection_hdl hdl, WsServer::message_ptr msg) {
        if (msg->get_opcode() != websocketpp::frame::opcode::binary) {
            // IObject 远程协议是二进制 MessagePack，文本帧视为协议违例。
            websocketpp::lib::error_code ec;
            server.get_con_from_hdl(hdl)->close(
                websocketpp::close::status::unsupported_data,
                "binary frames only", ec);
            return;
        }
        std::shared_ptr<ConnState> state;
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            const auto found = connections.find(hdl.lock().get());
            if (found == connections.end()) {
                return;  // 连接正在关闭中。
            }
            state = found->second;
        }
        // 拷贝一份字节并封回事件循环线程；shared_ptr 保证 ConnState 存活，
        // state->peer 判空覆盖"销毁任务已先执行"的边界。
        std::string payload = msg->get_payload();
        iobject::Post([state, payload = std::move(payload)]() {
            if (!state->peer) {
                return;
            }
            state->peer->ReceiveMessage(ByteView(
                reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
        });
    }
};

// =============================================================================
// 构造 / 析构
// =============================================================================

WebSocketServer::WebSocketServer(RuntimeBridgeRoot& bridgeRoot, Config config)
    : impl_(std::make_unique<Impl>()) {
    impl_->bridgeRoot = &bridgeRoot;
    impl_->config = std::move(config);

    // 屏蔽 websocketpp 自身的控制台日志，统一走 iobject Logger。
    impl_->server.get_alog().clear_channels(websocketpp::log::alevel::all);
    impl_->server.get_elog().clear_channels(websocketpp::log::elevel::all);

    impl_->server.init_asio(&impl_->io);
#ifndef _WIN32
    // POSIX 下仅用于 TIME_WAIT 复用；Windows 的 SO_REUSEADDR 语义是允许重复绑定
    // 同一端口（多个进程同时 LISTEN，连接落点不确定），故 Windows 保持默认独占，
    // 端口被占时 listen 失败，由下方的容错逻辑告警并禁用服务端。
    impl_->server.set_reuse_addr(true);
#endif
    // 协议层单条消息上限 1 MiB，传输层略放宽并整体封顶。
    impl_->server.set_max_message_size(2 * 1024 * 1024);

    impl_->server.set_open_handler(
        [impl = impl_.get()](websocketpp::connection_hdl hdl) { impl->onOpen(hdl); });
    impl_->server.set_close_handler(
        [impl = impl_.get()](websocketpp::connection_hdl hdl) { impl->onClose(hdl); });
    impl_->server.set_message_handler(
        [impl = impl_.get()](websocketpp::connection_hdl hdl, WsServer::message_ptr msg) {
            impl->onMessage(hdl, msg);
        });

    try {
        // 显式 IPv4 any，避免不同平台默认 endpoint 差异。
        asio::ip::tcp::endpoint endpoint(asio::ip::address_v4::any(), impl_->config.port);
        impl_->server.listen(endpoint);
    } catch (const std::exception& e) {
        impl_->log.Log(LogLevel::Error, "WebSocket",
                        std::string("监听端口失败: ") + e.what());
        return;  // 保持未运行状态，不影响宿主进程。
    }

    impl_->server.start_accept();
    impl_->thread = std::thread([impl = impl_.get()]() { impl->io.run(); });
    impl_->running.store(true);
    impl_->log.Log(LogLevel::Info, "WebSocket",
                    "服务端已启动: port=" + std::to_string(impl_->config.port)
                        + ", domain=" + impl_->config.domain);
}

WebSocketServer::~WebSocketServer() {
    Stop();
}

// =============================================================================
// 状态查询
// =============================================================================

bool WebSocketServer::IsRunning() const noexcept {
    return impl_ && impl_->running.load();
}

std::uint16_t WebSocketServer::Port() const noexcept {
    return impl_ ? impl_->config.port : 0;
}

const std::string& WebSocketServer::Domain() const noexcept {
    static const std::string kEmpty;
    return impl_ ? impl_->config.domain : kEmpty;
}

// =============================================================================
// 停止
// =============================================================================

void WebSocketServer::Stop() noexcept {
    if (!impl_) {
        return;
    }
    if (impl_->thread.joinable()) {
        // io_service::stop 线程安全；join 后 wspp 线程不再产生任何回调。
        impl_->io.stop();
        impl_->thread.join();
    }
    impl_->running.store(false);

    // wspp 线程已死，取出全部残留连接。
    std::vector<std::shared_ptr<ConnState>> states;
    {
        std::lock_guard<std::mutex> lock(impl_->connectionsMutex);
        states.reserve(impl_->connections.size());
        for (auto& [key, state] : impl_->connections) {
            states.push_back(std::move(state));
        }
        impl_->connections.clear();
    }
    if (states.empty()) {
        return;
    }
    if (iobject::IsOnLoopThread()) {
        // 当前就是循环线程：直接销毁（peer 析构会关闭底层 RuntimeSession）。
        for (auto& state : states) {
            state->peer.reset();
        }
    } else {
        // 封回循环线程串行销毁；若循环已停（进程退出），任务被丢弃，
        // shared_ptr 环随之泄漏——无害且不可观察。
        iobject::Post([states = std::move(states)]() {
            for (auto& state : states) {
                state->peer.reset();
            }
        });
    }
}

// =============================================================================
// 原生对象契约
// =============================================================================

bool WebSocketServer::ReadData(DataChannelView channel, DataReceiver receiver) const {
    if (channel == "Port") {
        const std::uint16_t port = Port();
        const std::array<std::uint8_t, 2> bytes{
            static_cast<std::uint8_t>((port >> 8) & 0xff),
            static_cast<std::uint8_t>(port & 0xff)};
        receiver(ByteView(bytes.data(), bytes.size()));
        return true;
    }
    return false;
}

} // namespace iobject
