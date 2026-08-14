#include <iobject/Runtime.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>

namespace {

class Counter final {
public:
    explicit Counter(std::int32_t initialValue) : value(initialValue) {}

    bool ReadData(iobject::DataChannelView channel, iobject::DataReceiver receiver) const {
        if (channel != "Value") {
            return false;
        }
        const std::span<const std::uint8_t> bytes(
            reinterpret_cast<const std::uint8_t*>(&value), sizeof(value));
        receiver(bytes);
        return true;
    }

    std::int32_t value = 0;
};

void PrintCounter(const iobject::IRuntimeObject* object, const char* title) {
    const Counter* counter = object == nullptr ? nullptr : object->As<Counter>();
    std::cout << "  " << title << "："
              << (counter == nullptr ? -1 : counter->value) << '\n';
}

} // namespace

int main() {
    using iobject::IRuntimeObject;
    using iobject::IRuntimeObjectPointer;
    using iobject::Runtime;
    using iobject::RuntimeObjectEvent;
    using iobject::RuntimeSubscription;

    std::cout << "透明指针节点演示\n";

    IRuntimeObject* first = Runtime::make<Counter>(10);
    IRuntimeObject* second = Runtime::make<Counter>(20);
    IRuntimeObject* parent = Runtime::make();
    IRuntimeObject* observer = Runtime::make();
    IRuntimeObjectPointer* pointer = Runtime::makePointer();

    std::cout << "\n=== 1. 空指针、绑定与 As<T>() ===\n";
    std::cout << "  初始已绑定：" << pointer->IsBound()
              << "，绑定对象：" << pointer->GetBindObject() << '\n';
    std::cout << "  识别为 IRuntimeObjectPointer："
              << (pointer->As<IRuntimeObjectPointer>() == pointer) << '\n';
    std::cout << "  绑定 first：" << pointer->Bind(first) << '\n';
    PrintCounter(pointer, "通过指针读取 first 的 Counter");

    std::cout << "\n=== 2. 调用时转发数据与拓扑 ===\n";
    bool read = pointer->ReadData("Value", [](iobject::ByteView bytes) {
        std::int32_t value = 0;
        std::memcpy(&value, bytes.data(), sizeof(value));
        std::cout << "  通过指针读取 Value 通道：" << value << '\n';
    });
    std::cout << "  ReadData 结果：" << read << '\n';
    std::cout << "  parent Connect pointer：" << parent->Connect("current", pointer) << '\n';
    std::cout << "  实际连接对象是否为 first："
              << (parent->GetChildItem("current") == first) << '\n';

    std::cout << "\n=== 3. 订阅与换绑均只作用于调用当次 ===\n";
    RuntimeSubscription subscription = observer->SubscribeEvent(
        pointer, "Changed", [first](const RuntimeObjectEvent& event) {
            std::cout << "  observer 收到 " << event.type
                      << "，source 是否为 first：" << (event.source == first) << '\n';
        });
    std::cout << "  以 pointer 作为 source 建立订阅：" << subscription.IsActive() << '\n';
    first->Publish("Changed");
    std::cout << "  换绑 second：" << pointer->Bind(second) << '\n';
    second->Publish("Changed");
    std::cout << "  换绑不会迁移旧订阅；上面第二次发布没有回调。\n";

    std::cout << "\n=== 4. 目标 Release 自动解绑，指针 Release 不影响目标 ===\n";
    second->Release();
    std::cout << "  second Release 后已绑定：" << pointer->IsBound()
              << "，绑定对象：" << pointer->GetBindObject() << '\n';
    pointer->Release();
    std::cout << "  pointer Release 后 As<IRuntimeObjectPointer>："
              << pointer->As<IRuntimeObjectPointer>() << '\n';
    PrintCounter(first, "first 仍可正常使用");

    subscription.Cancel();
    delete pointer;
    delete observer;
    delete parent;
    delete second;
    delete first;

    std::cout << "\n演示结束。\n";
    return 0;
}
