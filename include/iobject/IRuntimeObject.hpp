#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

namespace iobject {

class IRuntimeObject;
class RuntimeSubscription;

/// 子节点查询快照中的非拥有视图；对象指针会在 Disconnect、Release、覆盖、子节点销毁或父节点销毁后失效。
struct RuntimeChildView {
    std::string name;
    IRuntimeObject* object = nullptr;
};

using RuntimeChildList = std::vector<RuntimeChildView>;

using RuntimeEventType = std::string;
using RuntimeEventTypeView = std::string_view;
using DataChannel = std::string;
using DataChannelView = std::string_view;
using ByteView = std::span<const std::uint8_t>;
using ByteInput = std::span<const std::uint8_t>;
using DataReceiver = std::function<void(ByteView)>;

namespace RuntimeEventTypes {

#define IOBJECT_RUNTIME_EVENT(name) inline constexpr RuntimeEventTypeView name = #name;
IOBJECT_RUNTIME_EVENT(ChildConnected)
IOBJECT_RUNTIME_EVENT(ChildDisconnected)
IOBJECT_RUNTIME_EVENT(DataChannelChanged)
IOBJECT_RUNTIME_EVENT(Released)
#undef IOBJECT_RUNTIME_EVENT

} // namespace RuntimeEventTypes

// 已废弃：旧事件载荷曾使用 EventDataPtr 和继承体系。
// 新事件载荷统一使用 IRuntimeObject，并通过 IRuntimeObject::As<T>() 转换。
// class EventData {
// public:
//     virtual ~EventData() = default;
//
//     template<typename T>
//     const T* As() const noexcept {
//         static_assert(std::is_base_of_v<EventData, T>,
//                       "EventData::As<T>() requires T to derive from EventData.");
//         return dynamic_cast<const T*>(this);
//     }
// };
//
// using EventDataPtr = std::shared_ptr<const EventData>;

/// ChildConnected / ChildDisconnected 的数据；child 为非拥有视图。
struct ChildEventData final {
    ChildEventData(std::string childName, const IRuntimeObject* childObject)
        : name(std::move(childName)), child(childObject) {}

    std::string name;
    const IRuntimeObject* child = nullptr;
};

/// DataChannelChanged 的数据；channel 是发生逻辑变化的业务通道名称。
struct DataChannelChangedEventData final {
    explicit DataChannelChangedEventData(DataChannel changedChannel)
        : channel(std::move(changedChannel)) {}

    DataChannel channel;
};

struct RuntimeObjectEvent {
    RuntimeEventType type;
    const IRuntimeObject* source = nullptr;
    /// 非拥有只读视图，仅在当前同步事件回调期间有效。
    const IRuntimeObject* data = nullptr;
};

using EventHandler = std::function<void(const RuntimeObjectEvent&)>;
using EventHandlerId = std::size_t;
using EventCallback = EventHandler;
using SubscriptionId = EventHandlerId;
using RuntimeObjectEventCallback = EventHandler;
using RuntimeObjectSubscriptionId = SubscriptionId;

namespace detail {

RuntimeSubscription createRuntimeSubscription(void* control, std::size_t id);
void cancelRuntimeSubscription(void* control, std::size_t id) noexcept;
bool isRuntimeSubscriptionActive(void* control, std::size_t id) noexcept;

} // namespace detail

/// 对象间订阅的独占 RAII 句柄；析构或 Cancel() 会解除关系。
class RuntimeSubscription final {
public:
    RuntimeSubscription() = default;
    ~RuntimeSubscription() {
        Cancel();
    }

    RuntimeSubscription(const RuntimeSubscription&) = delete;
    RuntimeSubscription& operator=(const RuntimeSubscription&) = delete;

    RuntimeSubscription(RuntimeSubscription&& other) noexcept
        : control_(other.control_), id_(other.id_) {
        other.control_ = nullptr;
        other.id_ = 0;
    }

    RuntimeSubscription& operator=(RuntimeSubscription&& other) noexcept {
        if (this != &other) {
            Cancel();
            control_ = other.control_;
            id_ = other.id_;
            other.control_ = nullptr;
            other.id_ = 0;
        }
        return *this;
    }

    void Cancel() noexcept {
        if (control_ != nullptr && id_ != 0) {
            detail::cancelRuntimeSubscription(control_, id_);
            control_ = nullptr;
            id_ = 0;
        }
    }

    bool IsActive() const noexcept {
        return control_ != nullptr && id_ != 0
            && detail::isRuntimeSubscriptionActive(control_, id_);
    }

private:
    friend RuntimeSubscription detail::createRuntimeSubscription(void* control, std::size_t id);

    RuntimeSubscription(void* control, std::size_t id) noexcept : control_(control), id_(id) {}

    void* control_ = nullptr;
    std::size_t id_ = 0;
};

/// Non-template runtime object contract for tree structure and events.
class IRuntimeObject {
public:
    /// Runtime 创建的节点由调用方 delete；析构会自动解除全部相关拓扑。
    virtual ~IRuntimeObject() = default;

    /// 查询包装对象当前能暴露的 T；纯运行时节点、已 Release 节点或未登记类型返回 nullptr。
    template<typename T>
    T* As() noexcept {
        return static_cast<T*>(QueryType(std::type_index(typeid(T))));
    }

    /// const 查询不允许通过返回指针修改包装对象。
    template<typename T>
    const T* As() const noexcept {
        return static_cast<const T*>(QueryType(std::type_index(typeid(T))));
    }

    /// 使用 RuntimeEventTypes 中的内置常量或业务自定义字符串；空字符串不会注册处理器，事件类型大小写敏感。
    virtual EventHandlerId AddEventHandler(RuntimeEventTypeView type, EventHandler handler) = 0;
    virtual bool RemoveEventHandler(EventHandlerId id) = 0;
    /// 使用 RuntimeEventTypes 中的内置常量或业务自定义字符串；空字符串不会建立订阅。
    /// source 若为 IRuntimeObjectPointer，会在本次调用解引用其当前绑定目标；空指针节点不能建立订阅。
    virtual RuntimeSubscription Observe(IRuntimeObject* source, RuntimeEventTypeView type) = 0;
    /// 发布内置或业务自定义事件；空字符串不会投递，data 可为 nullptr。
    /// destroyDataAfterPublish 为 false 时仅借用 data，调用方继续负责其 delete。
    /// 为 true 时本次调用立即接管 data 的独占删除责任；完整同步派发结束、
    /// 未投递或处理器抛异常离开时都会 delete data。交付后不得再次使用或交付该指针。
    virtual void Publish(RuntimeEventTypeView type, IRuntimeObject* data = nullptr,
                         bool destroyDataAfterPublish = false) = 0;

    /// 只解除全部入边、出边并发送一次 Released；重复调用安全。
    /// Released 发生在节点内存失效前；source 仅可在当前回调中作地址身份比较，不应调用或保存。
    /// 不释放运行时节点或其承载的原生对象；调用方仍须 delete 节点。
    virtual void Release() noexcept = 0;

    /// 从业务通道同步读取不透明字节数据；字节视图仅在 receiver 调用期间有效。
    /// 空数据是成功结果；空通道、空 receiver、无读取能力或已 Release 返回 false。
    virtual bool ReadData(DataChannelView channel, DataReceiver receiver) const = 0;
    /// 向业务通道同步写入不透明字节数据；ByteInput 仅在本次调用期间有效。
    /// 成功只表示原生对象接受并处理了输入，不会自动发布 DataChannelChanged。
    /// 数据变化后的通知由业务方通过 Publish(RuntimeEventTypes::DataChannelChanged, ...) 显式完成。
    virtual bool WriteData(DataChannelView channel, ByteInput data) = 0;

    /// name 必须是非空且不含 '.' 的单层名称；child 为 nullptr、自环或会形成间接环时返回 false。
    /// child 若为 IRuntimeObjectPointer，会在本次调用解引用其当前绑定目标；空指针节点不能建立连接。
    /// 父节点只保存非拥有指针，绝不 delete child；调用方无需在销毁 child 前 Disconnect。
    /// 覆盖仅替换关联，不销毁旧子节点。
    virtual bool Connect(std::string name, IRuntimeObject* child, bool overwrite = false) = 0;
    /// 只接受非空且不含 '.' 的单层名称。
    virtual bool Disconnect(const std::string& name) = 0;
    /// 查询 '.' 分隔的相对向下路径；空路径、首尾点、连续点或任一层未命中时返回 nullptr。
    virtual IRuntimeObject* GetChildItem(const std::string& path) = 0;
    /// const 查询版本不允许经返回结果修改节点。
    virtual const IRuntimeObject* GetChildItem(const std::string& path) const = 0;
    /// 返回不暴露内部容器的只读子节点快照；其中非拥有指针会在 Disconnect、Release、覆盖或相关节点析构后失效。
    virtual RuntimeChildList GetChildren() const = 0;

protected:
    virtual void* QueryType(std::type_index type) noexcept = 0;
    virtual const void* QueryType(std::type_index type) const noexcept = 0;
};

} // namespace iobject
