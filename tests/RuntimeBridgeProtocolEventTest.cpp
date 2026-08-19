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

class Counter final {
public:
    void Increase() {
        ++value_;
        node_->Publish(iobject::RuntimeEventTypes::DataChannelChanged,
                       iobject::Runtime::make<iobject::DataChannelChangedEventData>("Value"),
                       true);
    }

    void Attach(iobject::IRuntimeObject* node) {
        node_ = node;
    }

private:
    iobject::IRuntimeObject* node_ = nullptr;
    int value_ = 0;
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
    auto* counter = new Counter();
    iobject::IRuntimeObject* node = iobject::Runtime::fromPtr(counter, true);
    counter->Attach(node);
    TEST_CHECK(loop.domain.RootAnchor()->Connect("Counter", node));

    const MsgPack found = loop.call("GetChildItem",
        {{"addr", MsgPack(loop.root)}, {"childId", MsgPack("Counter")}});
    const std::uint64_t addr = static_cast<std::uint64_t>(found["addr"].int64_value());
    TEST_CHECK(addr != 0);

    // 1. 订阅 DataChannelChanged：返回非零 subscription。
    const MsgPack subscribed = loop.call("SubscribeEvent",
        {{"addr", MsgPack(addr)}, {"type", MsgPack("DataChannelChanged")}});
    TEST_CHECK(subscribed["ok"].bool_value());
    const std::uint64_t subscription =
        static_cast<std::uint64_t>(subscribed["subscription"].int64_value());
    TEST_CHECK(subscription != 0);

    // 2. C++ 侧发布：事件下行帧带 event/subscription/addr/channel。
    const std::size_t framesBefore = loop.outbox.size();
    counter->Increase();
    TEST_CHECK(loop.outbox.size() == framesBefore + 1);
    const MsgPack event = loop.lastFrame();
    TEST_CHECK(event["event"].string_value() == "DataChannelChanged");
    TEST_CHECK(static_cast<std::uint64_t>(event["subscription"].int64_value()) == subscription);
    TEST_CHECK(static_cast<std::uint64_t>(event["addr"].int64_value()) == addr);
    TEST_CHECK(event["channel"].string_value() == "Value");

    // 3. CancelEvent：ok；再次取消同一订阅：SubscriptionInvalid。
    TEST_CHECK(loop.call("CancelEvent",
        {{"subscription", MsgPack(subscription)}})["ok"].bool_value());
    const std::size_t framesAfterCancel = loop.outbox.size();
    counter->Increase();
    TEST_CHECK(loop.outbox.size() == framesAfterCancel);  // 不再收到事件
    TEST_CHECK(errorCode(loop.call("CancelEvent",
        {{"subscription", MsgPack(subscription)}})) == "SubscriptionInvalid");

    // 4. 无效 addr 订阅：AddrInvalid。
    TEST_CHECK(errorCode(loop.call("SubscribeEvent",
        {{"addr", MsgPack(std::uint64_t(999999))}, {"type", MsgPack("X")}})) == "AddrInvalid");

    // 5. 订阅 Released：对象 Release 后收到事件帧，channel 为空字符串。
    const MsgPack releasedSub = loop.call("SubscribeEvent",
        {{"addr", MsgPack(addr)}, {"type", MsgPack("Released")}});
    TEST_CHECK(releasedSub["ok"].bool_value());
    const std::size_t beforeRelease = loop.outbox.size();
    node->Release();
    TEST_CHECK(loop.outbox.size() == beforeRelease + 1);
    const MsgPack released = loop.lastFrame();
    TEST_CHECK(released["event"].string_value() == "Released");
    TEST_CHECK(static_cast<std::uint64_t>(released["addr"].int64_value()) == addr);
    TEST_CHECK(released["channel"].string_value().empty());

    // 6. Release 后该 addr 的操作回 AddrInvalid。
    TEST_CHECK(errorCode(loop.call("ReadData",
        {{"addr", MsgPack(addr)}, {"channel", MsgPack("Value")}})) == "AddrInvalid");

    // 7. 关闭连接后不再产生任何帧。
    loop.peer->Close();
    const std::size_t framesAfterClose = loop.outbox.size();
    counter->Increase();
    TEST_CHECK(loop.outbox.size() == framesAfterClose);

    delete node;  // owned=true，随节点释放 counter。
    return 0;
}
