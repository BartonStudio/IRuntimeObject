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

    MsgPack getChildItem(std::uint64_t addr, const std::string& childId) {
        return roundtrip(request({{"op", MsgPack("GetChildItem")},
                                  {"addr", MsgPack(addr)},
                                  {"childId", MsgPack(childId)}}));
    }

    MsgPack getChildren(std::uint64_t addr) {
        return roundtrip(request({{"op", MsgPack("GetChildren")}, {"addr", MsgPack(addr)}}));
    }
};

std::string errorCode(const MsgPack& response) {
    return response["error"]["code"].string_value();
}

} // namespace

int main() {
    iobject::IRuntimeObject* keepAlive = nullptr;
    {
        Loopback loop;
        iobject::IRuntimeObject* player = iobject::Runtime::make();
        iobject::IRuntimeObject* decoder = iobject::Runtime::make();
        keepAlive = decoder;
        TEST_CHECK(loop.domain.RootAnchor()->Connect("Player", player));
        TEST_CHECK(player->Connect("Decoder", decoder));

        // 逐级解析：root -> Player -> Decoder，响应回显 childId。
        const MsgPack playerResponse = loop.getChildItem(loop.root, "Player");
        TEST_CHECK(playerResponse["ok"].bool_value());
        TEST_CHECK(playerResponse["childId"].string_value() == "Player");
        const std::uint64_t playerAddr =
            static_cast<std::uint64_t>(playerResponse["addr"].int64_value());
        TEST_CHECK(playerAddr != 0);

        const MsgPack decoderResponse = loop.getChildItem(playerAddr, "Decoder");
        TEST_CHECK(decoderResponse["ok"].bool_value());
        TEST_CHECK(decoderResponse["childId"].string_value() == "Decoder");
        TEST_CHECK(decoderResponse["addr"].int64_value() > 0);

        // 子对象不存在：ObjectNotFound。
        TEST_CHECK(errorCode(loop.getChildItem(loop.root, "Missing")) == "ObjectNotFound");

        // addr 无效：AddrInvalid。
        TEST_CHECK(errorCode(loop.getChildItem(999999, "Player")) == "AddrInvalid");

        // childId 含 '.' 或为空：MalformedMessage（协议规定 childId 是单层名称）。
        TEST_CHECK(errorCode(loop.getChildItem(loop.root, "Player.Decoder")) == "MalformedMessage");
        TEST_CHECK(errorCode(loop.getChildItem(loop.root, "")) == "MalformedMessage");

        // GetChildren 枚举：root -> [Player, WebSocket]（按 childId 字典序；
        // WebSocket 是 RuntimeDomain 内置的 WebSocket 服务端节点）。
        const MsgPack rootChildren = loop.getChildren(loop.root);
        TEST_CHECK(rootChildren["ok"].bool_value());
        TEST_CHECK(rootChildren["children"].is_array());
        const MsgPack::array& rootList = rootChildren["children"].array_items();
        TEST_CHECK(rootList.size() == 2);
        TEST_CHECK(rootList[0]["name"].string_value() == "Player");
        TEST_CHECK(rootList[0]["addr"].int64_value() > 0);
        TEST_CHECK(rootList[1]["name"].string_value() == "WebSocket");
        TEST_CHECK(rootList[1]["addr"].int64_value() > 0);

        // player -> [Decoder]
        const MsgPack playerChildren = loop.getChildren(playerAddr);
        TEST_CHECK(playerChildren["ok"].bool_value());
        const MsgPack::array& playerList = playerChildren["children"].array_items();
        TEST_CHECK(playerList.size() == 1);
        TEST_CHECK(playerList[0]["name"].string_value() == "Decoder");

        // decoder（叶子）-> 空 children
        const std::uint64_t decoderAddr =
            static_cast<std::uint64_t>(decoderResponse["addr"].int64_value());
        const MsgPack leafChildren = loop.getChildren(decoderAddr);
        TEST_CHECK(leafChildren["ok"].bool_value());
        TEST_CHECK(leafChildren["children"].array_items().empty());

        // addr 无效 -> AddrInvalid
        TEST_CHECK(errorCode(loop.getChildren(999999)) == "AddrInvalid");

        delete player;  // decoder 随 player 的拓扑解除而独立存活，稍后删除。
    }
    delete keepAlive;
    return 0;
}
