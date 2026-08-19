#include <iobject/RuntimeBridgeProtocol.hpp>

#include <iobject/RuntimeBridge.hpp>

#include <msgpack11.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <utility>

namespace iobject {
namespace {

using msgpack11::MsgPack;

constexpr std::size_t MaxMessageSize = 1024 * 1024;  // 协议建议的单条消息上限 1 MiB。

// msgpack11 按宽度区分整数类型；int64_value 对所有整数类型可用（已核实源码 static_cast）。
// 协议承诺数值不超过 2^53，int64 范围足够；负数拒绝。
bool asUint64(const MsgPack& value, std::uint64_t& out) {
    if (!(value.type() & MsgPack::INT)) {
        return false;
    }
    const std::int64_t signedValue = value.int64_value();
    if (signedValue < 0) {
        return false;
    }
    out = static_cast<std::uint64_t>(signedValue);
    return true;
}

bool asString(const MsgPack& value, std::string& out) {
    if (!value.is_string()) {
        return false;
    }
    out = value.string_value();
    return true;
}

MsgPack okResponse(std::uint64_t id, MsgPack::object fields = {}) {
    MsgPack::object object{{"id", MsgPack(id)}, {"ok", MsgPack(true)}};
    for (auto& [key, value] : fields) {
        object.emplace(key, std::move(value));
    }
    return MsgPack(std::move(object));
}

MsgPack errorResponse(std::uint64_t id, const char* code, const std::string& message) {
    return MsgPack(MsgPack::object{
        {"id", MsgPack(id)},
        {"ok", MsgPack(false)},
        {"error", MsgPack(MsgPack::object{
            {"code", MsgPack(code)},
            {"message", MsgPack(message)}})}});
}

} // namespace

struct RuntimeBridgePeer::Impl {
    RuntimeBridgeRoot* bridgeRoot = nullptr;  // 非拥有。
    std::string domain;
    RuntimeBridgePeer::SendCallback send;
    std::unique_ptr<RuntimeSession> session;
    std::set<std::uint64_t> subscriptions;
    bool open = true;

    void sendMessage(const MsgPack& message) {
        if (!open || !send) {
            return;
        }
        const std::string frame = message.dump();
        send(ByteView(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()));
    }

    void close() noexcept {
        if (!open) {
            return;
        }
        open = false;
        subscriptions.clear();
        if (session != nullptr) {
            session->Close();
            session.reset();
        }
    }

    void receive(ByteView message) {
        if (!open) {
            return;
        }
        if (message.size() > MaxMessageSize) {
            sendMessage(errorResponse(0, "MalformedMessage", "消息超过 1 MiB 上限"));
            close();
            return;
        }

        std::string parseError;
        const MsgPack request = MsgPack::parse(
            std::string(reinterpret_cast<const char*>(message.data()), message.size()),
            parseError);
        if (!parseError.empty() || !request.is_object()) {
            sendMessage(errorResponse(0, "MalformedMessage", "消息不是合法的 MessagePack map"));
            return;
        }

        std::uint64_t id = 0;
        if (!asUint64(request["id"], id) || id == 0) {
            sendMessage(errorResponse(0, "MalformedMessage", "缺少合法的 id 字段"));
            return;
        }
        std::string op;
        if (!asString(request["op"], op) || op.empty()) {
            sendMessage(errorResponse(id, "MalformedMessage", "缺少合法的 op 字段"));
            return;
        }

        if (session == nullptr && op != "Connect") {
            sendMessage(errorResponse(id, "SessionNotEstablished", "握手成功前只接受 Connect"));
            return;
        }

        if (op == "Connect") {
            handleConnect(id, request);
        } else if (op == "Close") {
            handleClose(id);
        } else {
            sendMessage(errorResponse(id, "UnknownOp", "未知操作: " + op));
        }
    }

    void handleConnect(std::uint64_t id, const MsgPack& request) {
        if (session != nullptr) {
            sendMessage(errorResponse(id, "OperationFailed", "会话已建立，忽略重复 Connect"));
            return;
        }
        std::string domain;
        if (!asString(request["domain"], domain) || domain.empty()) {
            sendMessage(errorResponse(id, "MalformedMessage", "缺少合法的 domain 字段"));
            return;
        }
        if (domain != this->domain) {
            sendMessage(errorResponse(id, "DomainNotFound", "域不存在: " + domain));
            close();
            return;
        }
        session = bridgeRoot->OpenSession();
        if (session == nullptr || session->RootObject() == 0) {
            session.reset();
            sendMessage(errorResponse(id, "OperationFailed", "桥接不可用"));
            return;
        }
        sendMessage(okResponse(id, {{"root", MsgPack(session->RootObject())}}));
    }

    void handleClose(std::uint64_t id) {
        sendMessage(okResponse(id));
        close();
    }
};

RuntimeBridgePeer::RuntimeBridgePeer(RuntimeBridgeRoot& bridgeRoot, std::string domain,
                                     SendCallback send)
    : impl_(std::make_unique<Impl>()) {
    impl_->bridgeRoot = &bridgeRoot;
    impl_->domain = std::move(domain);
    impl_->send = std::move(send);
}

RuntimeBridgePeer::~RuntimeBridgePeer() {
    impl_->close();
}

void RuntimeBridgePeer::ReceiveMessage(ByteView message) {
    impl_->receive(message);
}

void RuntimeBridgePeer::Close() noexcept {
    impl_->close();
}

bool RuntimeBridgePeer::IsOpen() const noexcept {
    return impl_->open;
}

} // namespace iobject
