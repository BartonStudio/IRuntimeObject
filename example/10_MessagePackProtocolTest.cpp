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

    MsgPack lastFrame() {
        std::string err;
        MsgPack frame = MsgPack::parse(outbox.back(), err);
        if (!err.empty()) {
            std::printf("  解析事件帧失败: %s\n", err.c_str());
        }
        return frame;
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

    // 5. 订阅 Lamp 的事件并发布，演示事件下行帧；随后取消订阅。
    const MsgPack sub = loop.call({{"op", MsgPack("SubscribeEvent")},
                                   {"addr", MsgPack(lampAddr)},
                                   {"type", MsgPack("DataChannelChanged")}});
    const std::uint64_t lampSub =
        static_cast<std::uint64_t>(sub["subscription"].int64_value());
    std::printf("4. SubscribeEvent subscription=%llu\n",
                static_cast<unsigned long long>(lampSub));
    lamp->Publish(iobject::RuntimeEventTypes::DataChannelChanged,
                  iobject::Runtime::make<iobject::DataChannelChangedEventData>("State"), true);
    const MsgPack event = loop.lastFrame();
    std::printf("   事件帧 event=%s channel=%s\n", event["event"].string_value().c_str(),
                event["channel"].string_value().c_str());
    loop.call({{"op", MsgPack("CancelEvent")}, {"subscription", MsgPack(lampSub)}});

    // 6. 通道同步：C++ 侧建立 Mirror 跟随 Lamp 的 State（本地能力，协议无此 op），
    //    远程订阅 Mirror 的事件；同步传播后远程收到的是 Mirror 的变化通知。
    iobject::IRuntimeObject* mirror = iobject::Runtime::make<Lamp>();
    loop.domain.RootAnchor()->Connect("Mirror", mirror);
    // 注意：RuntimeSubscription 是 RAII 句柄，必须持有它，临时对象析构会立即取消订阅。
    iobject::RuntimeSubscription sync = mirror->SubscribeChannel(lamp, "State");

    const MsgPack foundMirror = loop.call(
        {{"op", MsgPack("GetChildItem")}, {"addr", MsgPack(root)}, {"childId", MsgPack("Mirror")}});
    const std::uint64_t mirrorAddr =
        static_cast<std::uint64_t>(foundMirror["addr"].int64_value());
    loop.call({{"op", MsgPack("SubscribeEvent")},
               {"addr", MsgPack(mirrorAddr)},
               {"type", MsgPack("DataChannelChanged")}});

    // 远程把 Lamp 的 State 改写为 0；C++ 业务按规则显式发布变化事件。
    loop.call({{"op", MsgPack("WriteData")},
               {"addr", MsgPack(lampAddr)},
               {"channel", MsgPack("State")},
               {"data", MsgPack(MsgPack::binary{0})}});
    lamp->Publish(iobject::RuntimeEventTypes::DataChannelChanged,
                  iobject::Runtime::make<iobject::DataChannelChangedEventData>("State"), true);

    // 同步链路自动执行：读 Lamp -> 写 Mirror -> 发布 Mirror 的 DataChannelChanged。
    const MsgPack mirrorEvent = loop.lastFrame();
    std::printf("5. 通道同步：收到事件 addr=%llu（Mirror）event=%s channel=%s\n",
                static_cast<unsigned long long>(mirrorEvent["addr"].int64_value()),
                mirrorEvent["event"].string_value().c_str(),
                mirrorEvent["channel"].string_value().c_str());

    // 远程读 Mirror，验证同步写入的值。
    const MsgPack mirrorRead = loop.call({{"op", MsgPack("ReadData")},
                                          {"addr", MsgPack(mirrorAddr)},
                                          {"channel", MsgPack("State")}});
    std::printf("   ReadData Mirror.State -> %u（已跟随 Lamp 变为 0）\n",
                mirrorRead["data"].is_binary()
                    ? static_cast<unsigned>(mirrorRead["data"].binary_items()[0])
                    : 0u);

    // 7. 关闭。
    const MsgPack bye = loop.call({{"op", MsgPack("Close")}});
    std::printf("6. Close ok=%d IsOpen=%d\n", bye["ok"].bool_value() ? 1 : 0,
                loop.peer->IsOpen() ? 1 : 0);

    delete mirror;
    delete lamp;
    return 0;
}
