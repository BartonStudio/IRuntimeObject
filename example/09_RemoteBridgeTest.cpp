// 09_RemoteBridgeTest：演示 RuntimeDomain / RuntimeBridgeRoot / RuntimeSession 的用法。
// 会话方法逐一对应未来 JS 端接口；本示例在进程内直接调用，不涉及真实传输。
#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace {

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
        std::printf("  Lamp::WriteData State -> %s\n", on_ ? "on" : "off");
        return true;
    }

private:
    bool on_ = false;
};

} // namespace

int main() {
    // 1. 创建域；域自动持有根锚点与桥接入口。
    iobject::RuntimeDomain domain;

    // 2. C++ 侧把业务对象接入根锚点子树（只有这棵子树对远程可见）。
    iobject::IRuntimeObject* lamp = iobject::Runtime::make<Lamp>();
    domain.RootAnchor()->Connect("Lamp", lamp);
    std::printf("1. 根锚点已接入业务对象 Lamp\n");

    // 3. 打开一个远程会话（未来由传输层触发；句柄不透明、按会话独立）。
    std::unique_ptr<iobject::RuntimeSession> session = domain.BridgeRoot().OpenSession();
    const iobject::RemoteObjectHandle lampHandle = session->ResolveRootChild("Lamp");
    std::printf("2. ResolveRootChild(\"Lamp\") -> 句柄 %llu\n",
                static_cast<unsigned long long>(lampHandle));

    // 4. 远程读写数据通道。
    const std::array<std::uint8_t, 1> turnOn{1};
    std::printf("3. WriteData State=1：\n");
    session->WriteData(lampHandle, "State", turnOn);
    session->ReadData(lampHandle, "State", [](iobject::ByteView bytes) {
        std::printf("4. ReadData State -> %u\n", static_cast<unsigned>(bytes[0]));
    });

    // 5. 远程订阅事件；C++ 侧业务发布后经中继节点转发给会话回调。
    const std::uint64_t subscription = session->SubscribeEvent(
        lampHandle, iobject::RuntimeEventTypes::DataChannelChanged,
        [](const iobject::RemoteEventMessage& message) {
            std::printf("5. 收到事件 type=%s channel=%s source=%llu\n", message.type.c_str(),
                        message.channel.c_str(),
                        static_cast<unsigned long long>(message.source));
        });
    lamp->Publish(iobject::RuntimeEventTypes::DataChannelChanged,
                  iobject::Runtime::make<iobject::DataChannelChangedEventData>("State"), true);
    session->CancelEvent(subscription);

    // 6. 关闭会话：全部句柄与订阅失效；随后由 C++ 侧照常销毁对象。
    session->Close();
    std::printf("6. 会话已关闭，IsOpen=%d\n", session->IsOpen() ? 1 : 0);

    delete lamp;
    return 0;
}
