#include <iobject/Runtime.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

namespace {

class CounterState final {
public:
    CounterState(std::string objectName, std::int32_t initialState)
        : name_(std::move(objectName)), state_(initialState), replica_(0) {}

    bool ReadData(iobject::DataChannelView channel, iobject::DataReceiver receiver) const {
        if (!receiver) {
            return false;
        }
        if (channel == "State") {
            const std::array<std::uint8_t, 4> bytes = Encode(state_);
            receiver(iobject::ByteView(bytes.data(), bytes.size()));
            return true;
        }
        if (channel == "Replica") {
            const std::array<std::uint8_t, 4> bytes = Encode(replica_);
            receiver(iobject::ByteView(bytes.data(), bytes.size()));
            return true;
        }
        return false;
    }

    bool WriteData(iobject::DataChannelView channel, iobject::ByteInput data) {
        if (data.size() != 4) {
            return false;
        }
        if (channel == "State") {
            state_ = Decode(data);
        } else if (channel == "Replica") {
            replica_ = Decode(data);
        } else {
            return false;
        }

        std::cout << "  对象 " << name_ << " 写入通道 " << channel
                  << "，值=" << Decode(data) << '\n';
        return true;
    }

private:
    static std::array<std::uint8_t, 4> Encode(std::int32_t value) {
        const std::uint32_t bits = static_cast<std::uint32_t>(value);
        return {
            static_cast<std::uint8_t>(bits & 0xffU),
            static_cast<std::uint8_t>((bits >> 8U) & 0xffU),
            static_cast<std::uint8_t>((bits >> 16U) & 0xffU),
            static_cast<std::uint8_t>((bits >> 24U) & 0xffU),
        };
    }

    static std::int32_t Decode(iobject::ByteInput bytes) {
        const std::uint32_t bits = static_cast<std::uint32_t>(bytes[0])
            | (static_cast<std::uint32_t>(bytes[1]) << 8U)
            | (static_cast<std::uint32_t>(bytes[2]) << 16U)
            | (static_cast<std::uint32_t>(bytes[3]) << 24U);
        return static_cast<std::int32_t>(bits);
    }

    std::string name_;
    std::int32_t state_ = 0;
    std::int32_t replica_ = 0;
};

std::array<std::uint8_t, 4> Encode(std::int32_t value) {
    const std::uint32_t bits = static_cast<std::uint32_t>(value);
    return {
        static_cast<std::uint8_t>(bits & 0xffU),
        static_cast<std::uint8_t>((bits >> 8U) & 0xffU),
        static_cast<std::uint8_t>((bits >> 16U) & 0xffU),
        static_cast<std::uint8_t>((bits >> 24U) & 0xffU),
    };
}

void PublishChannelChanged(iobject::IRuntimeObject* source, const char* channel) {
    source->Publish(
        iobject::RuntimeEventTypes::DataChannelChanged,
        iobject::Runtime::make<iobject::DataChannelChangedEventData>(channel), true);
}

} // namespace

int main() {
    using iobject::IRuntimeObject;
    using iobject::Runtime;
    using iobject::RuntimeSubscription;

    std::cout << "SubscribeChannel 数据通道订阅演示\n";

    IRuntimeObject* objectA = Runtime::make<CounterState>("A", 10);
    IRuntimeObject* objectB = Runtime::make<CounterState>("B", 0);
    IRuntimeObject* objectC = Runtime::make<CounterState>("C", 0);
    IRuntimeObject* objectD = Runtime::make<CounterState>("D", 0);

    RuntimeSubscription sameName = objectB->SubscribeChannel(objectA, "State");
    RuntimeSubscription mapped = objectC->SubscribeChannel(objectA, "State", "Replica");
    RuntimeSubscription publicFunction = iobject::SubscribeChannel(
        objectB, "State", objectD, "State");

    std::cout << "\n=== 1. 建立同名、映射和公共四参数订阅 ===\n";
    std::cout << "  sameName 有效：" << sameName.IsActive() << '\n';
    std::cout << "  mapped 有效：" << mapped.IsActive() << '\n';
    std::cout << "  publicFunction 有效：" << publicFunction.IsActive() << '\n';

    std::cout << "\n=== 2. A 写入后显式发布变化，触发 B/C/D 链式同步 ===\n";
    const std::array<std::uint8_t, 4> value20 = Encode(20);
    const bool writeA = objectA->WriteData(
        "State", iobject::ByteInput(value20.data(), value20.size()));
    std::cout << "  A 写入 State=20 结果：" << (writeA ? "成功" : "失败")
              << "（WriteData 不自行发布事件）\n";
    PublishChannelChanged(objectA, "State");

    std::cout << "\n=== 3. 三次无效通知均不产生通道写入 ===\n";
    std::cout << "  3.1 DataChannelChanged 空 payload\n";
    objectA->Publish(iobject::RuntimeEventTypes::DataChannelChanged);
    std::cout << "  3.2 DataChannelChanged 错误载荷类型 ChildEventData\n";
    objectA->Publish(
        iobject::RuntimeEventTypes::DataChannelChanged,
        Runtime::make<iobject::ChildEventData>("错误载荷", objectB), true);
    std::cout << "  3.3 DataChannelChanged 通道名不匹配\n";
    PublishChannelChanged(objectA, "Unknown");
    std::cout << "  三次通知完成；以上没有新的通道写入输出。\n";

    std::cout << "\n=== 4. 取消 sameName 后 B 不再接收 A 的直接同步 ===\n";
    sameName.Cancel();
    std::cout << "  sameName 有效：" << sameName.IsActive() << '\n';
    const std::array<std::uint8_t, 4> value30 = Encode(30);
    objectA->WriteData("State", iobject::ByteInput(value30.data(), value30.size()));
    PublishChannelChanged(objectA, "State");
    std::cout << "  本次发布只保留 A -> C 的 Replica 映射，B 没有直接写入。\n";

    mapped.Cancel();
    publicFunction.Cancel();

    std::cout << "\n=== 5. A -> B -> C -> B 环路被重复节点截断 ===\n";
    RuntimeSubscription loopAB = objectB->SubscribeChannel(objectA, "State");
    RuntimeSubscription loopBC = objectC->SubscribeChannel(objectB, "State");
    RuntimeSubscription loopCB = objectB->SubscribeChannel(objectC, "State");
    std::cout << "  loopAB/loopBC/loopCB 有效：" << loopAB.IsActive() << '/'
              << loopBC.IsActive() << '/' << loopCB.IsActive() << '\n';
    const std::array<std::uint8_t, 4> value40 = Encode(40);
    objectA->WriteData("State", iobject::ByteInput(value40.data(), value40.size()));
    PublishChannelChanged(objectA, "State");
    std::cout << "  环路发布返回；重复节点之后不再继续同步。\n";

    std::cout << "\n=== 6. 释放订阅端后相关句柄失效 ===\n";
    objectC->Release();
    std::cout << "  C Release 后 loopBC 有效：" << loopBC.IsActive() << '\n';
    std::cout << "  C Release 后 loopCB 有效：" << loopCB.IsActive() << '\n';

    loopAB.Cancel();
    loopBC.Cancel();
    loopCB.Cancel();
    delete objectD;
    delete objectC;
    delete objectB;
    delete objectA;

    std::cout << "\n演示结束。\n";
    return 0;
}
