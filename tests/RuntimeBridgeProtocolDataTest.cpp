#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeBridgeProtocol.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <msgpack11.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using msgpack11::MsgPack;

class Device final {
public:
    bool ReadData(iobject::DataChannelView channel, iobject::DataReceiver receiver) const {
        if (channel != "State") {
            return false;
        }
        const std::array<std::uint8_t, 4> bytes{
            static_cast<std::uint8_t>(state_ & 0xFF),
            static_cast<std::uint8_t>((state_ >> 8) & 0xFF),
            static_cast<std::uint8_t>((state_ >> 16) & 0xFF),
            static_cast<std::uint8_t>((state_ >> 24) & 0xFF)};
        receiver(bytes);
        return true;
    }

    bool WriteData(iobject::DataChannelView channel, iobject::ByteInput data) {
        if (channel != "State" || data.size() != 4) {
            return false;
        }
        state_ = static_cast<std::uint32_t>(data[0])
               | (static_cast<std::uint32_t>(data[1]) << 8)
               | (static_cast<std::uint32_t>(data[2]) << 16)
               | (static_cast<std::uint32_t>(data[3]) << 24);
        return true;
    }

private:
    std::uint32_t state_ = 0;
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
    iobject::IRuntimeObject* device = iobject::Runtime::make<Device>();
    TEST_CHECK(loop.domain.RootAnchor()->Connect("Device", device));

    const MsgPack found = loop.call("GetChildItem",
        {{"addr", MsgPack(loop.root)}, {"childId", MsgPack("Device")}});
    TEST_CHECK(found["ok"].bool_value());
    const std::uint64_t addr = static_cast<std::uint64_t>(found["addr"].int64_value());
    TEST_CHECK(addr != 0);

    // 写入 300（小端四字节），再读回。
    const MsgPack written = loop.call("WriteData",
        {{"addr", MsgPack(addr)},
         {"channel", MsgPack("State")},
         {"data", MsgPack(MsgPack::binary{0x2C, 0x01, 0x00, 0x00})}});
    TEST_CHECK(written["ok"].bool_value());

    const MsgPack read = loop.call("ReadData",
        {{"addr", MsgPack(addr)}, {"channel", MsgPack("State")}});
    TEST_CHECK(read["ok"].bool_value());
    TEST_CHECK(read["data"].is_binary());
    const MsgPack::binary& bytes = read["data"].binary_items();
    TEST_CHECK(bytes.size() == 4);
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
    TEST_CHECK(value == 300);

    // 未知通道：OperationFailed。
    TEST_CHECK(errorCode(loop.call("ReadData",
        {{"addr", MsgPack(addr)}, {"channel", MsgPack("Missing")}})) == "OperationFailed");

    // addr 无效：AddrInvalid。
    TEST_CHECK(errorCode(loop.call("ReadData",
        {{"addr", MsgPack(std::uint64_t(999999))}, {"channel", MsgPack("State")}}))
        == "AddrInvalid");

    // data 字段不是二进制：MalformedMessage。
    TEST_CHECK(errorCode(loop.call("WriteData",
        {{"addr", MsgPack(addr)},
         {"channel", MsgPack("State")},
         {"data", MsgPack("not-binary")}})) == "MalformedMessage");

    delete device;
    return 0;
}
