#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>

namespace {

// 声明 BindRuntime 的原生对象：框架在节点构造后绑定 self，析构时以 nullptr 解绑。
class SelfAware final {
public:
    void BindRuntime(iobject::IRuntimeObject* self) {
        self_ = self;
    }

    iobject::IRuntimeObject* self() const {
        return self_;
    }

    bool Invoke(iobject::MethodView method, iobject::ByteInput args, iobject::DataReceiver result) {
        static_cast<void>(args);
        if (method == "Publish" && self_ != nullptr) {
            // 通过 self 指针发布事件（原生对象不持有节点指针时的目标能力）。
            self_->Publish(
                iobject::RuntimeEventTypes::DataChannelChanged,
                iobject::Runtime::make<iobject::DataChannelChangedEventData>("State"), true);
            result(iobject::ByteView{});
            return true;
        }
        return false;
    }

private:
    iobject::IRuntimeObject* self_ = nullptr;
};

} // namespace

int main() {
    // 非拥有包装（ref）：对象比节点活得久，用于验证解绑会清空 self。
    SelfAware obj;
    iobject::IRuntimeObject* node = iobject::Runtime::ref(obj);
    TEST_CHECK(obj.self() == node);  // 构造后已绑定

    // self 指针可用：经 self 发布事件并订阅到。
    int published = 0;
    iobject::RuntimeSubscription sub = node->SubscribeEvent(
        node, iobject::RuntimeEventTypes::DataChannelChanged,
        [&published](const iobject::RuntimeObjectEvent&) { ++published; });
    TEST_CHECK(sub.IsActive());
    TEST_CHECK(node->Invoke("Publish", {}, [](iobject::ByteView) {}));
    TEST_CHECK(published == 1);

    delete node;
    TEST_CHECK(obj.self() == nullptr);  // 析构时已解绑（对象仍存活）

    return 0;
}
