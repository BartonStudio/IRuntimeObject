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

class Service final {
public:
    bool Invoke(iobject::MethodView method, iobject::ByteInput args, iobject::DataReceiver result) {
        if (method == "Echo") {
            result(args);
            return true;
        }
        if (method == "Ping") {
            result(iobject::ByteView{});
            return true;
        }
        return false;
    }
};

struct Loopback {
    iobject::RuntimeDomain domain;
    std::vector<std::string> outbox;
    std::unique_ptr<iobject::RuntimeBridgePeer> peer;
    std::uint64_t root = 0;
    std::uint64_t nextId = 1;

    Loopback()
        : peer(std::make_unique<iobject::RuntimeBridgePeer>(
              domain.BridgeRoot(), "MainScene",
              [this](iobject::ByteView frame) {
                  outbox.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
              })) {
        const MsgPack response = roundtrip(request(
            {{"op", MsgPack("Connect")}, {"domain", MsgPack("MainScene")}}));
        TEST_CHECK(response["ok"].bool_value());
        root = static_cast<std::uint64_t>(response["root"].int64_value());
        TEST_CHECK(root != 0);
    }

    void send(const MsgPack& message) {
        const std::string frame = message.dump();
        peer->ReceiveMessage(
            iobject::ByteView(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()));
    }

    MsgPack lastFrame() {
        TEST_CHECK(!outbox.empty());
        std::string err;
        const MsgPack frame = MsgPack::parse(outbox.back(), err);
        TEST_CHECK(err.empty());
        return frame;
    }

    MsgPack request(MsgPack::object object) {
        object.emplace(MsgPack("id"), MsgPack(nextId++));
        return MsgPack(std::move(object));
    }

    MsgPack roundtrip(const MsgPack& message) {
        send(message);
        return lastFrame();
    }

    MsgPack call(const char* op, MsgPack::object fields) {
        MsgPack::object object{{"op", MsgPack(op)}};
        for (auto& [key, value] : fields) {
            object.emplace(key, std::move(value));
        }
        return roundtrip(request(std::move(object)));
    }
};

std::string errorCode(const MsgPack& response) {
    return response["error"]["code"].string_value();
}

} // namespace

int main() {
    Loopback loop;
    iobject::IRuntimeObject* service = iobject::Runtime::make<Service>();
    TEST_CHECK(loop.domain.RootAnchor()->Connect("Service", service));

    const MsgPack found = loop.call("GetChildItem",
        {{"addr", MsgPack(loop.root)}, {"childId", MsgPack("Service")}});
    TEST_CHECK(found["ok"].bool_value());
    const std::uint64_t addr = static_cast<std::uint64_t>(found["addr"].int64_value());
    TEST_CHECK(addr != 0);

    // Echo：args 原样回显为 result。
    const MsgPack echoed = loop.call("Invoke",
        {{"addr", MsgPack(addr)},
         {"method", MsgPack("Echo")},
         {"args", MsgPack(MsgPack::binary{0xAA, 0xBB, 0xCC})}});
    TEST_CHECK(echoed["ok"].bool_value());
    TEST_CHECK(echoed["result"].is_binary());
    const MsgPack::binary& echoBytes = echoed["result"].binary_items();
    TEST_CHECK(echoBytes.size() == 3);
    TEST_CHECK(echoBytes[0] == 0xAA && echoBytes[1] == 0xBB && echoBytes[2] == 0xCC);

    // Ping：无返回值，result 为空 bin 但字段存在。
    const MsgPack pinged = loop.call("Invoke",
        {{"addr", MsgPack(addr)}, {"method", MsgPack("Ping")}, {"args", MsgPack(MsgPack::binary{})}});
    TEST_CHECK(pinged["ok"].bool_value());
    TEST_CHECK(pinged["result"].is_binary());
    TEST_CHECK(pinged["result"].binary_items().empty());

    // 未知方法：OperationFailed。
    TEST_CHECK(errorCode(loop.call("Invoke",
        {{"addr", MsgPack(addr)},
         {"method", MsgPack("Missing")},
         {"args", MsgPack(MsgPack::binary{})}})) == "OperationFailed");

    // 空 method：MalformedMessage。
    TEST_CHECK(errorCode(loop.call("Invoke",
        {{"addr", MsgPack(addr)}, {"method", MsgPack("")}, {"args", MsgPack(MsgPack::binary{})}}))
        == "MalformedMessage");

    // args 非二进制：MalformedMessage。
    TEST_CHECK(errorCode(loop.call("Invoke",
        {{"addr", MsgPack(addr)}, {"method", MsgPack("Echo")}, {"args", MsgPack("not-binary")}}))
        == "MalformedMessage");

    // addr 无效：AddrInvalid。
    TEST_CHECK(errorCode(loop.call("Invoke",
        {{"addr", MsgPack(std::uint64_t(999999))},
         {"method", MsgPack("Echo")},
         {"args", MsgPack(MsgPack::binary{})}})) == "AddrInvalid");

    delete service;
    return 0;
}
