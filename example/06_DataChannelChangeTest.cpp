#include <iobject/Runtime.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

class CounterState {
public:
    explicit CounterState(std::int32_t value) : value_(value) {}

    bool ReadData(iobject::DataChannelView channel, iobject::DataReceiver receiver) const {
        if (channel != "State") {
            return false;
        }

        const std::array<std::uint8_t, 4> bytes = Encode(value_);
        receiver(iobject::ByteView(bytes.data(), bytes.size()));
        return true;
    }

    bool WriteData(iobject::DataChannelView channel, iobject::ByteInput data) {
        if (channel != "State" || data.size() != 4) {
            return false;
        }

        value_ = Decode(data);
        return true;
    }

private:
    static std::array<std::uint8_t, 4> Encode(std::int32_t value) {
        return {
            static_cast<std::uint8_t>(value & 0xff),
            static_cast<std::uint8_t>((value >> 8) & 0xff),
            static_cast<std::uint8_t>((value >> 16) & 0xff),
            static_cast<std::uint8_t>((value >> 24) & 0xff),
        };
    }

    static std::int32_t Decode(iobject::ByteInput bytes) {
        const std::uint32_t value = static_cast<std::uint32_t>(bytes[0])
            | (static_cast<std::uint32_t>(bytes[1]) << 8)
            | (static_cast<std::uint32_t>(bytes[2]) << 16)
            | (static_cast<std::uint32_t>(bytes[3]) << 24);
        return static_cast<std::int32_t>(value);
    }

    std::int32_t value_ = 0;
};

std::array<std::uint8_t, 4> Encode(std::int32_t value) {
    return {
        static_cast<std::uint8_t>(value & 0xff),
        static_cast<std::uint8_t>((value >> 8) & 0xff),
        static_cast<std::uint8_t>((value >> 16) & 0xff),
        static_cast<std::uint8_t>((value >> 24) & 0xff),
    };
}

std::int32_t Decode(iobject::ByteView bytes) {
    if (bytes.size() != 4) {
        return 0;
    }
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
    return static_cast<std::int32_t>(value);
}

void PrintResult(const char* title, bool success) {
    std::cout << "  " << title << "：" << (success ? "成功" : "失败") << '\n';
}

} // namespace

int main() {
    using iobject::DataChannelChangedEventData;
    using iobject::IRuntimeObject;
    using iobject::Runtime;
    using iobject::RuntimeObjectEvent;
    using iobject::RuntimeSubscription;

    std::cout << "数据通道变化通知（模式 A）演示\n";

    IRuntimeObject* objectA = Runtime::make();
    IRuntimeObject* objectB = Runtime::make<CounterState>(10);
    IRuntimeObject* plainObject = Runtime::make();

    RuntimeSubscription subscription = objectA->SubscribeEvent(
        objectB, iobject::RuntimeEventTypes::DataChannelChanged,
        [objectB](const RuntimeObjectEvent& event) {
            if (event.data == nullptr) {
                std::cout << "  DataChannelChanged 未携带数据。\n";
                return;
            }
            const DataChannelChangedEventData* data =
                event.data->As<DataChannelChangedEventData>();
            if (data == nullptr) {
                std::cout << "  未取得 DataChannelChangedEventData。\n";
                return;
            }
            if (data->channel != "State") {
                std::cout << "  忽略通道 " << data->channel << " 的变化通知。\n";
                return;
            }

            std::cout << "  A 收到 State 变化通知，主动读取 B：";
            const bool read = objectB->ReadData("State", [](iobject::ByteView bytes) {
                std::cout << Decode(bytes) << '\n';
            });
            if (!read) {
                std::cout << "读取失败\n";
            }
        });

    std::cout << "\n1. 建立 A 观察 B 的 DataChannelChanged\n";
    std::cout << "  订阅有效：" << subscription.IsActive() << '\n';

    std::cout << "\n2. WriteData 只写入，不自动通知\n";
    const std::array<std::uint8_t, 4> value20 = Encode(20);
    const bool writeState = objectB->WriteData("State", iobject::ByteInput(value20.data(), value20.size()));
    PrintResult("B 写入 State=20", writeState);
    std::cout << "  此处没有 A 的输出，因为业务尚未显式发布变化通知。\n";

    std::cout << "\n3. 业务显式发布 State 的变化通知\n";
    objectB->Publish(
        iobject::RuntimeEventTypes::DataChannelChanged,
        Runtime::make<DataChannelChangedEventData>("State"),
        true);

    std::cout << "\n4. 非目标通道的变化可以安全忽略\n";
    objectB->Publish(
        iobject::RuntimeEventTypes::DataChannelChanged,
        Runtime::make<DataChannelChangedEventData>("Metadata"),
        true);

    std::cout << "\n5. 未支持写入的节点返回失败\n";
    const bool writePlain = plainObject->WriteData("State", iobject::ByteInput(value20.data(), value20.size()));
    PrintResult("纯运行时节点写入 State", writePlain);

    subscription.Cancel();
    delete plainObject;
    delete objectB;
    delete objectA;

    std::cout << "\n演示结束。\n";
    return 0;
}
