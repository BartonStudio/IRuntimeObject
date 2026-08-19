#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeBridgeProtocol.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <msgpack11.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using msgpack11::MsgPack;

// 进程内回环：出站帧收集到 outbox，供解析断言。
struct Loopback {
    iobject::RuntimeDomain domain;
    std::vector<std::string> outbox;
    std::unique_ptr<iobject::RuntimeBridgePeer> peer;

    Loopback()
        : peer(std::make_unique<iobject::RuntimeBridgePeer>(
              domain.BridgeRoot(), "MainScene",
              [this](iobject::ByteView frame) {
                  outbox.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
              })) {}

    void send(const MsgPack& message) {
        const std::string frame = message.dump();
        peer->ReceiveMessage(
            iobject::ByteView(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()));
    }

    void sendRaw(const void* data, std::size_t size) {
        peer->ReceiveMessage(iobject::ByteView(static_cast<const std::uint8_t*>(data), size));
    }

    MsgPack lastFrame() {
        TEST_CHECK(!outbox.empty());
        std::string err;
        const MsgPack frame = MsgPack::parse(outbox.back(), err);
        TEST_CHECK(err.empty());
        return frame;
    }

    MsgPack roundtrip(const MsgPack& request) {
        send(request);
        return lastFrame();
    }

    MsgPack request(std::uint64_t id, const char* op, MsgPack::object fields = {}) {
        MsgPack::object object{{"id", MsgPack(id)}, {"op", MsgPack(op)}};
        for (auto& [key, value] : fields) {
            object.emplace(key, std::move(value));
        }
        return MsgPack(std::move(object));
    }
};

std::string errorCode(const MsgPack& response) {
    return response["error"]["code"].string_value();
}

} // namespace

int main() {
    // 1. 握手成功：返回非零 root addr。
    {
        Loopback loop;
        const MsgPack response =
            loop.roundtrip(loop.request(1, "Connect", {{"domain", MsgPack("MainScene")}}));
        TEST_CHECK(response["ok"].bool_value());
        TEST_CHECK(response["root"].int64_value() > 0);
        TEST_CHECK(loop.peer->IsOpen());
    }

    // 2. 域名不匹配：DomainNotFound，随后连接关闭，后续消息被忽略。
    {
        Loopback loop;
        const MsgPack response =
            loop.roundtrip(loop.request(1, "Connect", {{"domain", MsgPack("Wrong")}}));
        TEST_CHECK(!response["ok"].bool_value());
        TEST_CHECK(errorCode(response) == "DomainNotFound");
        TEST_CHECK(!loop.peer->IsOpen());
        const std::size_t frames = loop.outbox.size();
        loop.send(loop.request(2, "Connect", {{"domain", MsgPack("MainScene")}}));
        TEST_CHECK(loop.outbox.size() == frames);
    }

    // 3. 握手前发送其他操作：SessionNotEstablished。
    {
        Loopback loop;
        const MsgPack response = loop.roundtrip(loop.request(1, "GetChildItem"));
        TEST_CHECK(!response["ok"].bool_value());
        TEST_CHECK(errorCode(response) == "SessionNotEstablished");
    }

    // 4. 非法字节流：MalformedMessage，id 为 0。
    {
        Loopback loop;
        const std::uint8_t garbage[2] = {0xC1, 0xFF};  // 0xC1 是 MessagePack 保留字节
        loop.sendRaw(garbage, sizeof(garbage));
        const MsgPack response = loop.lastFrame();
        TEST_CHECK(!response["ok"].bool_value());
        TEST_CHECK(response["id"].int64_value() == 0);
        TEST_CHECK(errorCode(response) == "MalformedMessage");
    }

    // 5. 非 map 消息：MalformedMessage，id 为 0。
    {
        Loopback loop;
        loop.send(MsgPack(std::uint64_t(42)));
        TEST_CHECK(errorCode(loop.lastFrame()) == "MalformedMessage");
    }

    // 6. 缺 id：MalformedMessage 且 id 为 0；缺 op：MalformedMessage 且回显 id。
    {
        Loopback loop;
        loop.send(MsgPack(MsgPack::object{{"op", MsgPack("Connect")}}));
        const MsgPack noId = loop.lastFrame();
        TEST_CHECK(errorCode(noId) == "MalformedMessage");
        TEST_CHECK(noId["id"].int64_value() == 0);

        loop.send(MsgPack(MsgPack::object{{"id", MsgPack(std::uint64_t(7))}}));
        const MsgPack noOp = loop.lastFrame();
        TEST_CHECK(errorCode(noOp) == "MalformedMessage");
        TEST_CHECK(noOp["id"].int64_value() == 7);
    }

    // 7. 重复 Connect：OperationFailed。
    {
        Loopback loop;
        loop.roundtrip(loop.request(1, "Connect", {{"domain", MsgPack("MainScene")}}));
        const MsgPack again =
            loop.roundtrip(loop.request(2, "Connect", {{"domain", MsgPack("MainScene")}}));
        TEST_CHECK(errorCode(again) == "OperationFailed");
    }

    // 8. 未知操作：UnknownOp。
    {
        Loopback loop;
        loop.roundtrip(loop.request(1, "Connect", {{"domain", MsgPack("MainScene")}}));
        const MsgPack response = loop.roundtrip(loop.request(2, "Fly"));
        TEST_CHECK(errorCode(response) == "UnknownOp");
    }

    // 9. 超长消息（> 1 MiB）：MalformedMessage 并关闭连接。
    {
        Loopback loop;
        const MsgPack huge =
            loop.request(1, "Connect", {{"domain", MsgPack(std::string(2 * 1024 * 1024, 'x'))}});
        loop.send(huge);
        const MsgPack response = loop.lastFrame();
        TEST_CHECK(errorCode(response) == "MalformedMessage");
        TEST_CHECK(!loop.peer->IsOpen());
    }

    // 10. Close：先回 ok 再关闭；之后消息被忽略。
    {
        Loopback loop;
        loop.roundtrip(loop.request(1, "Connect", {{"domain", MsgPack("MainScene")}}));
        const MsgPack response = loop.roundtrip(loop.request(2, "Close"));
        TEST_CHECK(response["ok"].bool_value());
        TEST_CHECK(!loop.peer->IsOpen());
        const std::size_t frames = loop.outbox.size();
        loop.send(loop.request(3, "Close"));
        TEST_CHECK(loop.outbox.size() == frames);
    }
    return 0;
}
