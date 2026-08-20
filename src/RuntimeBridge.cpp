#include <iobject/RuntimeBridge.hpp>

#include <iobject/Runtime.hpp>

#include <map>
#include <utility>

namespace iobject {

struct RuntimeSession::Impl {
    struct EventEntry {
        IRuntimeObject* source = nullptr;
        RuntimeSubscription subscription;
    };

    IRuntimeObject* rootAnchor = nullptr;  // 非拥有，由 RuntimeDomain 持有。
    RemoteObjectHandle rootHandle = 0;     // 构造时登记的根锚点句柄。
    IRuntimeObject* relay = nullptr;       // 拥有：会话的中继订阅节点。
    bool open = true;
    RemoteObjectHandle nextHandle = 1;
    std::uint64_t nextSubscriptionId = 1;
    std::map<IRuntimeObject*, RemoteObjectHandle> handlesByObject;
    std::map<RemoteObjectHandle, IRuntimeObject*> objectsByHandle;
    std::map<IRuntimeObject*, RuntimeSubscription> releaseWatchByObject;
    std::map<std::uint64_t, EventEntry> eventSubscriptions;

    RemoteObjectHandle registerObject(IRuntimeObject* object) {
        if (!open || object == nullptr) {
            return 0;
        }
        const auto found = handlesByObject.find(object);
        if (found != handlesByObject.end()) {
            return found->second;
        }

        // 监视 Released：对象退出 IRuntimeObject 系统后句柄立即失效。
        // 先建立订阅并验证成功，再统一登记三张表；订阅失败则不登记句柄，避免失去失效保护。
        RuntimeSubscription watch = relay->SubscribeEvent(
            object, RuntimeEventTypes::Released,
            [this, object](const RuntimeObjectEvent&) {
                invalidate(object);
            });
        if (!watch.IsActive()) {
            return 0;
        }

        const RemoteObjectHandle handle = nextHandle++;
        handlesByObject.emplace(object, handle);
        objectsByHandle.emplace(handle, object);
        releaseWatchByObject.emplace(object, std::move(watch));
        return handle;
    }

    IRuntimeObject* resolve(RemoteObjectHandle handle) const {
        if (!open) {
            return nullptr;
        }
        const auto found = objectsByHandle.find(handle);
        return found == objectsByHandle.end() ? nullptr : found->second;
    }

    void invalidate(IRuntimeObject* object) noexcept {
        const auto found = handlesByObject.find(object);
        if (found == handlesByObject.end()) {
            return;
        }
        objectsByHandle.erase(found->second);
        handlesByObject.erase(found);
        releaseWatchByObject.erase(object);  // 析构 RuntimeSubscription，幂等 Cancel。
        // 注意：此处不得抹除 eventSubscriptions 中该对象的订阅。
        // 本回调运行在 Released 同步派发期间，提前抹除会让远程端收不到 Released；
        // 内核在 Released 投递后会自动解除源侧订阅，残留条目变为不活跃，
        // 由 CancelEvent/close 时清理，安全无害。
    }

    void close() noexcept {
        if (!open) {
            return;
        }
        open = false;
        eventSubscriptions.clear();    // 句柄析构自动 Cancel。
        releaseWatchByObject.clear();
        handlesByObject.clear();
        objectsByHandle.clear();
        if (relay != nullptr) {
            relay->Release();
            delete relay;
            relay = nullptr;
        }
    }
};

RuntimeSession::RuntimeSession(IRuntimeObject* rootAnchor, IRuntimeObject* relay)
    : impl_(std::make_unique<Impl>()) {
    impl_->rootAnchor = rootAnchor;
    impl_->relay = relay;
    impl_->rootHandle = impl_->registerObject(rootAnchor);
}

RuntimeSession::~RuntimeSession() {
    impl_->close();
}

RemoteObjectHandle RuntimeSession::ResolveRootChild(const std::string& path) {
    if (!impl_->open) {
        return 0;
    }
    return impl_->registerObject(impl_->rootAnchor->GetChildItem(path));
}

RemoteObjectHandle RuntimeSession::ResolveChild(RemoteObjectHandle handle, const std::string& path) {
    IRuntimeObject* object = impl_->resolve(handle);
    return object == nullptr ? 0 : impl_->registerObject(object->GetChildItem(path));
}

bool RuntimeSession::ReadData(RemoteObjectHandle handle, DataChannelView channel,
                              DataReceiver receiver) const {
    IRuntimeObject* object = impl_->resolve(handle);
    return object != nullptr && object->ReadData(channel, std::move(receiver));
}

bool RuntimeSession::WriteData(RemoteObjectHandle handle, DataChannelView channel, ByteInput data) {
    IRuntimeObject* object = impl_->resolve(handle);
    return object != nullptr && object->WriteData(channel, data);
}

std::uint64_t RuntimeSession::SubscribeEvent(RemoteObjectHandle handle, RuntimeEventTypeView type,
                                             RemoteEventCallback callback) {
    if (!impl_->open || !callback) {
        return 0;
    }
    IRuntimeObject* source = impl_->resolve(handle);
    if (source == nullptr) {
        return 0;
    }

    RuntimeSubscription subscription = impl_->relay->SubscribeEvent(
        source, type,
        [handle, callback = std::move(callback)](const RuntimeObjectEvent& event) {
            RemoteEventMessage message;
            message.source = handle;
            message.type = event.type;
            if (event.data != nullptr) {
                if (const DataChannelChangedEventData* changed =
                        event.data->As<DataChannelChangedEventData>()) {
                    message.channel = changed->channel;
                }
            }
            // 数据快照：通道变化通知在派发窗口内同步读取源对象；读失败则不携带 data。
            if (!message.channel.empty() && event.source != nullptr) {
                std::vector<std::uint8_t> bytes;
                if (event.source->ReadData(message.channel, [&bytes](ByteView view) {
                        bytes.assign(view.begin(), view.end());
                    })) {
                    message.data = std::move(bytes);
                }
            }
            callback(message);
        });
    if (!subscription.IsActive()) {
        return 0;
    }

    const std::uint64_t id = impl_->nextSubscriptionId++;
    impl_->eventSubscriptions.emplace(id, Impl::EventEntry{source, std::move(subscription)});
    return id;
}

void RuntimeSession::CancelEvent(std::uint64_t subscriptionId) noexcept {
    impl_->eventSubscriptions.erase(subscriptionId);  // 句柄析构自动 Cancel。
}

void RuntimeSession::Close() noexcept {
    impl_->close();
}

bool RuntimeSession::IsOpen() const noexcept {
    return impl_->open;
}

RemoteObjectHandle RuntimeSession::RootObject() const noexcept {
    return impl_->open ? impl_->rootHandle : 0;
}

bool RuntimeSession::HasObject(RemoteObjectHandle handle) const noexcept {
    return impl_->open
        && impl_->objectsByHandle.find(handle) != impl_->objectsByHandle.end();
}

std::unique_ptr<RuntimeSession> RuntimeBridgeRoot::OpenSession() {
    if (rootAnchor_ == nullptr) {
        return nullptr;
    }
    // 用 unique_ptr 守护 relay：RuntimeSession 构造抛异常时自动回收，避免泄漏。
    std::unique_ptr<IRuntimeObject> relayGuard(Runtime::make());
    return std::unique_ptr<RuntimeSession>(new RuntimeSession(rootAnchor_, relayGuard.release()));
}

} // namespace iobject
