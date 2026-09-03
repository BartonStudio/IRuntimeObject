#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <cstdint>
#include <memory>

namespace {

class Counter final {
public:
    void Increase() {
        ++value_;
        PublishChanged();
    }

    bool ReadData(iobject::DataChannelView channel, iobject::DataReceiver receiver) const {
        if (channel != "Value") {
            return false;
        }
        const std::uint8_t bytes[1] = {static_cast<std::uint8_t>(value_)};
        receiver(iobject::ByteView(bytes, 1));
        return true;
    }

    void PublishChanged() {
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

} // namespace

int main() {
    iobject::RuntimeDomain domain;
    auto* counter = new Counter();
    iobject::IRuntimeObject* node = iobject::Runtime::fromPtr(counter, true);
    counter->Attach(node);
    TEST_CHECK(domain.RootAnchor()->Connect("Counter", node));

    std::unique_ptr<iobject::RuntimeSession> session = domain.BridgeRoot().OpenSession();
    const iobject::RemoteObjectHandle handle = session->ResolveRootChild("Counter");
    TEST_CHECK(handle != 0);

    // 订阅 DataChannelChanged：转发 source 句柄、类型与 channel。
    int eventCount = 0;
    iobject::RemoteObjectHandle lastSource = 0;
    iobject::DataChannel lastChannel;
    const std::uint64_t subscription = session->SubscribeEvent(
        handle, iobject::RuntimeEventTypes::DataChannelChanged,
        [&](const iobject::RemoteEventMessage& message) {
            ++eventCount;
            lastSource = message.source;
            lastChannel = message.channel;
        });
    TEST_CHECK(subscription != 0);

    counter->Increase();
    TEST_CHECK(eventCount == 1);
    TEST_CHECK(lastSource == handle);
    TEST_CHECK(lastChannel == "Value");

    // CancelEvent 后不再收到。
    session->CancelEvent(subscription);
    counter->Increase();
    TEST_CHECK(eventCount == 1);

    // 对象 Release 后句柄失效，事件订阅随之解除。
    const std::uint64_t again = session->SubscribeEvent(
        handle, iobject::RuntimeEventTypes::DataChannelChanged,
        [&](const iobject::RemoteEventMessage&) { ++eventCount; });
    TEST_CHECK(again != 0);

    // 远程端能收到 Released 通知：恰好一次，source 为对象句柄。
    int releasedCount = 0;
    iobject::RemoteObjectHandle releasedSource = 0;
    iobject::RuntimeEventType releasedType;
    const std::uint64_t releasedSub = session->SubscribeEvent(
        handle, iobject::RuntimeEventTypes::Released,
        [&](const iobject::RemoteEventMessage& message) {
            ++releasedCount;
            releasedSource = message.source;
            releasedType = message.type;
        });
    TEST_CHECK(releasedSub != 0);

    node->Release();
    TEST_CHECK(releasedCount == 1);
    TEST_CHECK(releasedSource == handle);
    TEST_CHECK(releasedType == iobject::RuntimeEventTypes::Released);
    TEST_CHECK(session->ResolveChild(handle, "Anything") == 0);
    TEST_CHECK(session->ReadData(handle, "Value", [](iobject::ByteView) {}) == false);
    counter->Increase();  // 节点已 Release，Publish 不再投递，也不应崩溃。
    TEST_CHECK(eventCount == 1);

    session->Close();
    delete node;  // owned=true，随节点 delete 释放 counter。
    return 0;
}
