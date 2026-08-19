// 10_MessagePackProtocolTest：演示 RuntimeBridgePeer 的 MessagePack 协议往返。
// 本示例在进程内回环：直接调用 ReceiveMessage 喂请求、在 SendCallback 里解析响应。
#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeBridgeProtocol.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <msgpack11.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using msgpack11::MsgPack;

class Lamp final {
public:
    bool ReadData(iobject::DataChannelView channel, iobject::DataReceiver receiver) const {
        if (channel != "State") {
            return false;
        }
        const std::uint8_t bytes[1] = {static_cast<std::uint8_t>(on_ ? 1 : 0)};
        receiver(iobject::ByteView(bytes, 1));
        return true;
    }

    bool WriteData(iobject::DataChannelView channel, iobject::ByteInput data) {
        if (channel != "State" || data.size() != 1) {
            return false;
        }
        on_ = data[0] != 0;
        return true;
    }

private:
    bool on_ = false;
};

struct Loopback {
    iobject::RuntimeDomain domain;
    std::vector<std::string> outbox;
    std::unique_ptr<iobject::RuntimeBridgePeer> peer;
    std::uint64_t nextId = 1;

    Loopback()
        : peer(std::make_unique<iobject::RuntimeBridgePeer>(
              domain.BridgeRoot(), "MainScene",
              [this](iobject::ByteView frame) {
                  outbox.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
              })) {}

    MsgPack call(MsgPack::object object) {
        object.emplace(MsgPack("id"), MsgPack(nextId++));
        const std::string frame = MsgPack(std::move(object)).dump();
        peer->ReceiveMessage(
            iobject::ByteView(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()));
        std::string err;
        MsgPack response = MsgPack::parse(outbox.back(), err);
        if (!err.empty()) {
            std::printf("  解析响应失败: %s\n", err.c_str());
        }
        return response;
    }
};

} // namespace

int main() {
    Loopback loop;

    // 1. C++ 侧接入业务对象。
    iobject::IRuntimeObject* lamp = iobject::Runtime::make<Lamp>();
    loop.domain.RootAnchor()->Connect("Lamp", lamp);

    // 2. 握手：获得根锚点 addr。
    const MsgPack hello = loop.call({{"op", MsgPack("Connect")}, {"domain", MsgPack("MainScene")}});
    const std::uint64_t root = static_cast<std::uint64_t>(hello["root"].int64_value());
    std::printf("1. Connect ok=%d root=%llu\n", hello["ok"].bool_value() ? 1 : 0,
                static_cast<unsigned long long>(root));

    // 3. 逐级发现对象。
    const MsgPack found = loop.call(
        {{"op", MsgPack("GetChildItem")}, {"addr", MsgPack(root)}, {"childId", MsgPack("Lamp")}});
    const std::uint64_t lampAddr = static_cast<std::uint64_t>(found["addr"].int64_value());
    std::printf("2. GetChildItem childId=%s addr=%llu\n",
                found["childId"].string_value().c_str(),
                static_cast<unsigned long long>(lampAddr));

    // 4. 写入通道再读回。
    loop.call({{"op", MsgPack("WriteData")},
               {"addr", MsgPack(lampAddr)},
               {"channel", MsgPack("State")},
               {"data", MsgPack(MsgPack::binary{1})}});
    const MsgPack read = loop.call(
        {{"op", MsgPack("ReadData")}, {"addr", MsgPack(lampAddr)}, {"channel", MsgPack("State")}});
    std::printf("3. ReadData State -> %u\n",
                read["data"].is_binary() ? static_cast<unsigned>(read["data"].binary_items()[0]) : 0u);

    // 5. 订阅事件并发布。
    const MsgPack sub = loop.call({{"op", MsgPack("SubscribeEvent")},
                                   {"addr", MsgPack(lampAddr)},
                                   {"type", MsgPack("DataChannelChanged")}});
    std::printf("4. SubscribeEvent subscription=%llu\n",
                static_cast<unsigned long long>(sub["subscription"].int64_value()));
    lamp->Publish(iobject::RuntimeEventTypes::DataChannelChanged,
                  iobject::Runtime::make<iobject::DataChannelChangedEventData>("State"), true);
    std::string eventError;
    const MsgPack event = MsgPack::parse(loop.outbox.back(), eventError);
    if (!eventError.empty()) {
        std::printf("  解析事件帧失败: %s\n", eventError.c_str());
    }
    std::printf("5. 事件帧 event=%s channel=%s\n", event["event"].string_value().c_str(),
                event["channel"].string_value().c_str());

    // 6. 关闭。
    const MsgPack bye = loop.call({{"op", MsgPack("Close")}});
    std::printf("6. Close ok=%d IsOpen=%d\n", bye["ok"].bool_value() ? 1 : 0,
                loop.peer->IsOpen() ? 1 : 0);

    delete lamp;
    return 0;
}
