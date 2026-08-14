#include <iobject/Runtime.hpp>

#include <iostream>

namespace {

class HealthChangedEventData final {
public:
    HealthChangedEventData(int previousHealth, int currentHealth)
        : oldHealth(previousHealth), newHealth(currentHealth) {}

    int oldHealth = 0;
    int newHealth = 0;
};

class TrackingEventData final {
public:
    explicit TrackingEventData(int value) : value(value) {}
    ~TrackingEventData() {
        std::cout << "  TrackingEventData 已析构。\n";
    }

    int value = 0;
};

void PrintChildEvent(const iobject::RuntimeObjectEvent& event) {
    if (event.data == nullptr) {
        std::cout << "  ChildConnected 未携带数据。\n";
        return;
    }

    const iobject::ChildEventData* data = event.data->As<iobject::ChildEventData>();
    if (data == nullptr) {
        std::cout << "  未取得 ChildEventData。\n";
        return;
    }

    std::cout << "  收到 " << event.type << "，名称=" << data->name
              << "，child 是否有效地址：" << (data->child != nullptr) << '\n';
}

} // namespace

int main() {
    using iobject::IRuntimeObject;
    using iobject::Runtime;
    using iobject::RuntimeObjectEvent;
    using iobject::RuntimeSubscription;

    std::cout << "对象间事件订阅演示\n";

    IRuntimeObject* objectA = Runtime::make();
    IRuntimeObject* objectB = Runtime::make();
    IRuntimeObject* objectC = Runtime::make();

    std::cout << "\n=== 1. 注册 B 的 ChildConnected 回调 ===\n";
    RuntimeSubscription childSubscription = objectA->SubscribeEvent(
        objectB, iobject::RuntimeEventTypes::ChildConnected,
        [](const RuntimeObjectEvent& event) {
            PrintChildEvent(event);
        });
    std::cout << "  ChildConnected 订阅有效：" << childSubscription.IsActive() << '\n';
    objectB->Connect("weapon", objectC);

    std::cout << "\n=== 2. 自定义字符串事件携带普通包装对象 ===\n";
    RuntimeSubscription healthSubscription = objectA->SubscribeEvent(
        objectB, "HealthChanged",
        [](const RuntimeObjectEvent& event) {
            if (event.data == nullptr) {
                std::cout << "  HealthChanged 未携带数据。\n";
                return;
            }
            const HealthChangedEventData* data = event.data->As<HealthChangedEventData>();
            if (data != nullptr) {
                std::cout << "  收到 HealthChanged：" << data->oldHealth
                          << " -> " << data->newHealth << '\n';
            }
        });
    std::cout << "  HealthChanged 订阅有效：" << healthSubscription.IsActive() << '\n';
    objectB->Publish("HealthChanged", Runtime::make<HealthChangedEventData>(100, 80), true);

    std::cout << "\n=== 3. true 会在全部同步处理器完成后析构载荷 ===\n";
    RuntimeSubscription trackingSubscription = objectA->SubscribeEvent(
        objectB, "Tracking",
        [](const RuntimeObjectEvent& event) {
            const TrackingEventData* data = event.data == nullptr
                ? nullptr
                : event.data->As<TrackingEventData>();
            std::cout << "  A 在回调中读取 TrackingEventData："
                      << (data == nullptr ? -1 : data->value) << '\n';
        });
    objectB->Publish("Tracking", Runtime::make<TrackingEventData>(42), true);
    std::cout << "  Publish 返回后，载荷已经析构。\n";
    trackingSubscription.Cancel();

    std::cout << "\n=== 4. 空事件字符串不会建立关系或发布 ===\n";
    RuntimeSubscription emptySubscription = objectA->SubscribeEvent(
        objectB, "", [](const RuntimeObjectEvent&) {});
    std::cout << "  空事件订阅有效：" << emptySubscription.IsActive() << '\n';
    objectB->Publish("");

    std::cout << "\n=== 5. 事件发布环会输出诊断并截断 ===\n";
    RuntimeSubscription loopSubscription = objectA->SubscribeEvent(
        objectB, "Loop",
        [objectB](const RuntimeObjectEvent&) {
            std::cout << "  A 收到 Loop，尝试让 B 再次发布 Loop。\n";
            objectB->Publish("Loop");
        });
    std::cout << "  Loop 订阅有效：" << loopSubscription.IsActive() << '\n';
    objectB->Publish("Loop");
    std::cout << "  环已被截断，程序继续执行。\n";
    loopSubscription.Cancel();

    std::cout << "\n=== 6. Release 发送一次 Released，随后 delete 不重复发送 ===\n";
    RuntimeSubscription releasedSubscription = objectA->SubscribeEvent(
        objectB, iobject::RuntimeEventTypes::Released,
        [objectB](const RuntimeObjectEvent& event) {
            std::cout << "  收到 Released，source 是否为 B：" << (event.source == objectB) << '\n';
        });
    std::cout << "  Released 订阅有效：" << releasedSubscription.IsActive() << '\n';
    objectB->Release();
    std::cout << "  B Release 后 Released 订阅有效：" << releasedSubscription.IsActive() << '\n';
    delete objectB;
    std::cout << "  B delete 完成；没有第二次 Released 输出。\n";

    std::cout << "\n=== 7. 未先 Release 的节点在 delete 时发送一次 Released ===\n";
    IRuntimeObject* objectD = Runtime::make();
    RuntimeSubscription directDeleteSubscription = objectA->SubscribeEvent(
        objectD, iobject::RuntimeEventTypes::Released,
        [objectD](const RuntimeObjectEvent& event) {
            std::cout << "  收到直接 delete 的 Released，source 是否为 D："
                      << (event.source == objectD) << '\n';
        });
    std::cout << "  直接 delete 的订阅有效：" << directDeleteSubscription.IsActive() << '\n';
    delete objectD;
    std::cout << "  D delete 后订阅有效：" << directDeleteSubscription.IsActive() << '\n';

    delete objectC;
    delete objectA;

    std::cout << "\n演示结束。\n";
    return 0;
}
