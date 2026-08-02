#include <iobject/Runtime.hpp>

#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace iobject {
namespace {

bool isSingleChildName(const std::string& name) {
    return !name.empty() && name.find('.') == std::string::npos;
}

class RuntimeObject;
class RuntimeObjectPointer;

IRuntimeObject* resolveRuntimeObject(IRuntimeObject* object) noexcept;
const IRuntimeObject* resolveRuntimeObject(const IRuntimeObject* object) noexcept;

enum class RuntimeObjectState {
    Active,
    Releasing,
    Released,
};

constexpr std::size_t MaxEventPublishDepth = 32;
constexpr std::size_t MaxEventPublishesPerChain = 128;

struct EventDispatchToken {
    const IRuntimeObject* source = nullptr;
    RuntimeEventType type;
};

struct RuntimeEventDispatchContext {
    std::vector<EventDispatchToken> activePath;
    std::size_t publishCount = 0;
};

thread_local RuntimeEventDispatchContext* currentEventDispatchContext = nullptr;

void reportEventDispatchTruncation(const char* reason, const RuntimeObjectEvent& event,
                                   const RuntimeEventDispatchContext& context) {
    std::cerr << "[IObject] 事件派发截断：" << reason
              << "，type=" << event.type
              << "，当前深度=" << context.activePath.size()
              << "，已发布次数=" << context.publishCount << '\n';
}

class EventDispatchScope final {
public:
    explicit EventDispatchScope(const RuntimeObjectEvent& event)
        : previousContext_(currentEventDispatchContext) {
        if (previousContext_ == nullptr) {
            ownedContext_ = std::make_unique<RuntimeEventDispatchContext>();
            currentEventDispatchContext = ownedContext_.get();
        }

        RuntimeEventDispatchContext& context = *currentEventDispatchContext;
        for (const EventDispatchToken& token : context.activePath) {
            if (token.source == event.source && token.type == event.type) {
                reportEventDispatchTruncation("检测到事件发布环", event, context);
                return;
            }
        }
        if (context.activePath.size() >= MaxEventPublishDepth) {
            reportEventDispatchTruncation("超过最大嵌套深度 32", event, context);
            return;
        }
        if (context.publishCount >= MaxEventPublishesPerChain) {
            reportEventDispatchTruncation("超过单链路最大发布次数 128", event, context);
            return;
        }

        ++context.publishCount;
        context.activePath.push_back({event.source, event.type});
        entered_ = true;
    }

    ~EventDispatchScope() {
        if (entered_) {
            currentEventDispatchContext->activePath.pop_back();
        }
        if (ownedContext_) {
            currentEventDispatchContext = previousContext_;
        }
    }

    bool entered() const noexcept {
        return entered_;
    }

private:
    RuntimeEventDispatchContext* previousContext_ = nullptr;
    std::unique_ptr<RuntimeEventDispatchContext> ownedContext_;
    bool entered_ = false;
};

struct TopologyEdge {
    RuntimeObject* parent = nullptr;
    std::string name;
    IRuntimeObject* child = nullptr;
};

struct DetachedTopology {
    std::vector<TopologyEdge> incoming;
    std::vector<TopologyEdge> outgoing;
};

struct EventSubscription {
    RuntimeObject* subscriber = nullptr;
    RuntimeObject* source = nullptr;
    RuntimeEventType type;
};

class RuntimeTopology {
public:
    bool connect(RuntimeObject* parent, std::string name, IRuntimeObject* child, bool overwrite);
    bool disconnect(RuntimeObject* parent, const std::string& name);
    IRuntimeObject* getChildItem(const RuntimeObject* parent, const std::string& name) const;
    RuntimeChildList getChildren(const RuntimeObject* parent) const;
    DetachedTopology detachTopology(RuntimeObject* object);

    RuntimeSubscription observe(RuntimeObject* subscriber, IRuntimeObject* source, RuntimeEventTypeView type);
    void cancelSubscription(std::size_t id) noexcept;
    bool isSubscriptionActive(std::size_t id) const noexcept;
    void publish(RuntimeObject* source, const RuntimeObjectEvent& event);
    void bindPointer(RuntimeObjectPointer* pointer, RuntimeObject* object);
    void unbindPointer(RuntimeObjectPointer* pointer, RuntimeObject* object) noexcept;
    void unbindPointers(RuntimeObject* object) noexcept;

private:
    friend class RuntimeObject;

    bool wouldCreateCycle(const RuntimeObject* parent, IRuntimeObject* child) const;
    void rebuildIncoming();
    void removeSubscription(std::size_t id) noexcept;
    void removeSubscriptionsFor(RuntimeObject* object) noexcept;

    std::map<RuntimeObject*, std::map<std::string, IRuntimeObject*>> childrenByParent_;
    std::map<IRuntimeObject*, std::vector<TopologyEdge>> parentsByChild_;
    std::size_t nextSubscriptionId_ = 1;
    std::map<std::size_t, EventSubscription> subscriptionsById_;
    std::map<RuntimeObject*, std::set<std::size_t>> subscriptionsBySubscriber_;
    std::map<std::pair<RuntimeObject*, RuntimeEventType>, std::set<std::size_t>> subscriptionsBySource_;
    std::map<RuntimeObject*, std::set<RuntimeObjectPointer*>> pointersByTarget_;
};

RuntimeTopology* runtimeTopology() {
    static RuntimeTopology* const topology = new RuntimeTopology();
    return topology;
}

class RuntimeEventDispatcher final {
public:
    EventHandlerId Add(RuntimeEventTypeView type, EventHandler handler) {
        if (type.empty() || !handler) {
            return 0;
        }
        const EventHandlerId id = nextHandlerId_++;
        handlers_.emplace(id, HandlerEntry{RuntimeEventType(type), std::move(handler)});
        return id;
    }

    bool Remove(EventHandlerId id) {
        return id != 0 && handlers_.erase(id) != 0;
    }

    void Deliver(const RuntimeObjectEvent& event) {
        std::vector<EventHandler> handlers;
        for (const auto& [id, entry] : handlers_) {
            static_cast<void>(id);
            if (entry.type == event.type) {
                handlers.push_back(entry.handler);
            }
        }
        for (const EventHandler& handler : handlers) {
            handler(event);
        }
    }

    void Clear() noexcept {
        handlers_.clear();
    }

private:
    struct HandlerEntry {
        RuntimeEventType type;
        EventHandler handler;
    };

    EventHandlerId nextHandlerId_ = 1;
    std::map<EventHandlerId, HandlerEntry> handlers_;
};

class RuntimeObject final : public IRuntimeObject {
    friend class RuntimeObjectPointer;
public:
    RuntimeObject(detail::RuntimeObjectBridge bridge, RuntimeTopology* topology)
        : lifetime_(std::move(bridge.lifetime)), object_(bridge.object), types_(bridge.types),
          readData_(std::move(bridge.readData)), writeData_(std::move(bridge.writeData)), topology_(topology) {}

    ~RuntimeObject() override {
        releaseInternal();
    }

    EventHandlerId AddEventHandler(RuntimeEventTypeView type, EventHandler handler) override {
        return state_ == RuntimeObjectState::Active
            ? eventDispatcher_.Add(type, std::move(handler))
            : 0;
    }

    bool RemoveEventHandler(EventHandlerId id) override {
        return state_ == RuntimeObjectState::Active && eventDispatcher_.Remove(id);
    }

    RuntimeSubscription Observe(IRuntimeObject* source, RuntimeEventTypeView type) override {
        source = resolveRuntimeObject(source);
        return state_ == RuntimeObjectState::Active
            ? topology_->observe(this, source, type)
            : RuntimeSubscription();
    }

    void Publish(RuntimeEventTypeView type, IRuntimeObject* data,
                 bool destroyDataAfterPublish) override {
        std::unique_ptr<IRuntimeObject> ownedData(destroyDataAfterPublish ? data : nullptr);
        if (state_ == RuntimeObjectState::Active && !type.empty()) {
            publishEvent({RuntimeEventType(type), this, data});
        }
    }

    void Release() noexcept override {
        releaseInternal();
    }

    bool ReadData(DataChannelView channel, DataReceiver receiver) const override {
        if (state_ != RuntimeObjectState::Active || channel.empty() || !receiver || !readData_) {
            return false;
        }
        return readData_(object_, channel, std::move(receiver));
    }

    bool WriteData(DataChannelView channel, ByteInput data) override {
        if (state_ != RuntimeObjectState::Active || channel.empty() || !writeData_) {
            return false;
        }
        return writeData_(object_, channel, data);
    }

    bool Connect(std::string name, IRuntimeObject* child, bool overwrite) override {
        child = resolveRuntimeObject(child);
        if (state_ != RuntimeObjectState::Active || !isSingleChildName(name)
            || child == nullptr || !canConnectTo(child)) {
            return false;
        }

        IRuntimeObject* previous = topology_->getChildItem(this, name);
        if (previous != nullptr && !overwrite) {
            return false;
        }
        if (previous == child) {
            return true;
        }
        if (topology_->wouldCreateCycle(this, child)) {
            return false;
        }

        if (previous != nullptr) {
            topology_->connect(this, name, child, true);
            publishChildEvent(RuntimeEventTypes::ChildDisconnected, name, previous);
            publishChildEvent(RuntimeEventTypes::ChildConnected, std::move(name), child);
            return true;
        }
        topology_->connect(this, name, child, false);
        publishChildEvent(RuntimeEventTypes::ChildConnected, std::move(name), child);
        return true;
    }

    bool Disconnect(const std::string& name) override {
        if (state_ != RuntimeObjectState::Active || !isSingleChildName(name)) {
            return false;
        }
        IRuntimeObject* child = topology_->getChildItem(this, name);
        if (child == nullptr || !topology_->disconnect(this, name)) {
            return false;
        }
        publishChildEvent(RuntimeEventTypes::ChildDisconnected, name, child);
        return true;
    }

    IRuntimeObject* GetChildItem(const std::string& path) override {
        if (state_ != RuntimeObjectState::Active || path.empty()) {
            return nullptr;
        }

        IRuntimeObject* current = this;
        std::size_t segmentStart = 0;
        while (segmentStart < path.size()) {
            const std::size_t separator = path.find('.', segmentStart);
            const std::size_t segmentLength = separator == std::string::npos
                ? path.size() - segmentStart
                : separator - segmentStart;
            if (segmentLength == 0) {
                return nullptr;
            }

            const std::string segment = path.substr(segmentStart, segmentLength);
            current = current == this
                ? topology_->getChildItem(this, segment)
                : current->GetChildItem(segment);
            if (current == nullptr) {
                return nullptr;
            }
            if (separator == std::string::npos) {
                return current;
            }
            segmentStart = separator + 1;
        }
        return nullptr;
    }

    const IRuntimeObject* GetChildItem(const std::string& path) const override {
        if (state_ != RuntimeObjectState::Active || path.empty()) {
            return nullptr;
        }

        const IRuntimeObject* current = this;
        std::size_t segmentStart = 0;
        while (segmentStart < path.size()) {
            const std::size_t separator = path.find('.', segmentStart);
            const std::size_t segmentLength = separator == std::string::npos
                ? path.size() - segmentStart
                : separator - segmentStart;
            if (segmentLength == 0) {
                return nullptr;
            }

            const std::string segment = path.substr(segmentStart, segmentLength);
            current = current == this
                ? topology_->getChildItem(this, segment)
                : current->GetChildItem(segment);
            if (current == nullptr) {
                return nullptr;
            }
            if (separator == std::string::npos) {
                return current;
            }
            segmentStart = separator + 1;
        }
        return nullptr;
    }

    RuntimeChildList GetChildren() const override {
        return state_ == RuntimeObjectState::Active ? topology_->getChildren(this) : RuntimeChildList();
    }

protected:
    void* QueryType(std::type_index type) noexcept override {
        if (state_ != RuntimeObjectState::Active || object_ == nullptr || types_ == nullptr) {
            return nullptr;
        }
        const auto found = types_->converters.find(type);
        return found == types_->converters.end() ? nullptr : found->second.mutableQuery(object_);
    }

    const void* QueryType(std::type_index type) const noexcept override {
        if (state_ != RuntimeObjectState::Active || object_ == nullptr || types_ == nullptr) {
            return nullptr;
        }
        const auto found = types_->converters.find(type);
        return found == types_->converters.end() ? nullptr : found->second.constQuery(object_);
    }

public:
    friend class RuntimeTopology;

    bool isActive() const noexcept {
        return state_ == RuntimeObjectState::Active;
    }

    RuntimeTopology* topology() const noexcept {
        return topology_;
    }

    void deliverEvent(const RuntimeObjectEvent& event) {
        if (state_ == RuntimeObjectState::Active) {
            eventDispatcher_.Deliver(event);
        }
    }

private:
    void publishEvent(const RuntimeObjectEvent& event) {
        EventDispatchScope dispatchScope(event);
        if (!dispatchScope.entered()) {
            return;
        }
        eventDispatcher_.Deliver(event);
        topology_->publish(this, event);
    }

    void publishChildEvent(RuntimeEventTypeView type, std::string name, const IRuntimeObject* child) {
        std::unique_ptr<IRuntimeObject> data(Runtime::make<ChildEventData>(std::move(name), child));
        publishEvent({RuntimeEventType(type), this, data.get()});
    }

    static bool canConnectTo(IRuntimeObject* child) {
        RuntimeObject* runtimeChild = dynamic_cast<RuntimeObject*>(child);
        return runtimeChild == nullptr || runtimeChild->isActive();
    }

    void releaseInternal() noexcept {
        if (state_ != RuntimeObjectState::Active) {
            return;
        }

        state_ = RuntimeObjectState::Releasing;
        topology_->unbindPointers(this);
        const DetachedTopology detached = topology_->detachTopology(this);
        for (const TopologyEdge& edge : detached.incoming) {
            try {
                edge.parent->publishChildEvent(RuntimeEventTypes::ChildDisconnected, edge.name, this);
            } catch (...) {
            }
        }
        for (const TopologyEdge& edge : detached.outgoing) {
            try {
                publishChildEvent(RuntimeEventTypes::ChildDisconnected, edge.name, edge.child);
            } catch (...) {
            }
        }
        try {
            publishEvent({RuntimeEventType(RuntimeEventTypes::Released), this, {}});
        } catch (...) {
        }
        topology_->removeSubscriptionsFor(this);
        eventDispatcher_.Clear();
        state_ = RuntimeObjectState::Released;
    }

    std::shared_ptr<void> lifetime_;
    void* object_ = nullptr;
    const detail::TypeDescription* types_ = nullptr;
    std::function<bool(const void*, DataChannelView, DataReceiver)> readData_;
    std::function<bool(void*, DataChannelView, ByteInput)> writeData_;
    RuntimeTopology* topology_;
    RuntimeObjectState state_ = RuntimeObjectState::Active;
    RuntimeEventDispatcher eventDispatcher_;
};

class RuntimeObjectPointer final : public IRuntimeObjectPointer {
    friend class RuntimeTopology;
public:
    explicit RuntimeObjectPointer(RuntimeTopology* topology) : topology_(topology) {}
    ~RuntimeObjectPointer() override { Unbind(); }

    bool Bind(IRuntimeObject* object) override {
        if (released_ || object == nullptr) {
            if (object == nullptr) { Unbind(); return true; }
            return false;
        }
        RuntimeObject* target = dynamic_cast<RuntimeObject*>(object);
        if (target == nullptr || target == reinterpret_cast<RuntimeObject*>(this)
            || !target->isActive() || target->topology() != topology_) {
            return false;
        }
        if (target_ == target) return true;
        Unbind();
        target_ = target;
        topology_->bindPointer(this, target_);
        return true;
    }

    void Unbind() noexcept override {
        if (target_ != nullptr) {
            topology_->unbindPointer(this, target_);
            target_ = nullptr;
        }
    }

    IRuntimeObject* GetBindObject() noexcept override { return target_ && target_->isActive() ? target_ : nullptr; }
    const IRuntimeObject* GetBindObject() const noexcept override { return target_ && target_->isActive() ? target_ : nullptr; }
    bool IsBound() const noexcept override { return GetBindObject() != nullptr; }

    EventHandlerId AddEventHandler(RuntimeEventTypeView t, EventHandler h) override { return target_ ? target_->AddEventHandler(t, std::move(h)) : 0; }
    bool RemoveEventHandler(EventHandlerId id) override { return target_ ? target_->RemoveEventHandler(id) : false; }
    RuntimeSubscription Observe(IRuntimeObject* source, RuntimeEventTypeView t) override { return target_ ? target_->Observe(source, t) : RuntimeSubscription(); }
    void Publish(RuntimeEventTypeView t, IRuntimeObject* data, bool destroy) override { if (target_) target_->Publish(t, data, destroy); else if (destroy) delete data; }
    void Release() noexcept override { Unbind(); released_ = true; }
    bool ReadData(DataChannelView c, DataReceiver r) const override { return target_ ? target_->ReadData(c, std::move(r)) : false; }
    bool WriteData(DataChannelView c, ByteInput d) override { return target_ ? target_->WriteData(c, d) : false; }
    bool Connect(std::string n, IRuntimeObject* c, bool o) override { return target_ ? target_->Connect(std::move(n), c, o) : false; }
    bool Disconnect(const std::string& n) override { return target_ ? target_->Disconnect(n) : false; }
    IRuntimeObject* GetChildItem(const std::string& p) override { return target_ ? target_->GetChildItem(p) : nullptr; }
    const IRuntimeObject* GetChildItem(const std::string& p) const override { return target_ ? target_->GetChildItem(p) : nullptr; }
    RuntimeChildList GetChildren() const override { return target_ ? target_->GetChildren() : RuntimeChildList(); }

protected:
    void* QueryType(std::type_index type) noexcept override {
        if (released_) return nullptr;
        if (type == std::type_index(typeid(IRuntimeObjectPointer))) return this;
        return target_ ? target_->QueryType(type) : nullptr;
    }
    const void* QueryType(std::type_index type) const noexcept override {
        if (released_) return nullptr;
        if (type == std::type_index(typeid(IRuntimeObjectPointer))) return this;
        return target_ ? target_->QueryType(type) : nullptr;
    }

private:
    RuntimeTopology* topology_;
    RuntimeObject* target_ = nullptr;
    bool released_ = false;
};

IRuntimeObject* resolveRuntimeObject(IRuntimeObject* object) noexcept {
    if (auto* pointer = dynamic_cast<IRuntimeObjectPointer*>(object)) return pointer->GetBindObject();
    return object;
}
const IRuntimeObject* resolveRuntimeObject(const IRuntimeObject* object) noexcept {
    if (auto* pointer = dynamic_cast<const IRuntimeObjectPointer*>(object)) return pointer->GetBindObject();
    return object;
}

void RuntimeTopology::bindPointer(RuntimeObjectPointer* pointer, RuntimeObject* object) {
    pointersByTarget_[object].insert(pointer);
}
void RuntimeTopology::unbindPointer(RuntimeObjectPointer* pointer, RuntimeObject* object) noexcept {
    auto found = pointersByTarget_.find(object);
    if (found != pointersByTarget_.end()) {
        found->second.erase(pointer);
        if (found->second.empty()) pointersByTarget_.erase(found);
    }
}
void RuntimeTopology::unbindPointers(RuntimeObject* object) noexcept {
    auto found = pointersByTarget_.find(object);
    if (found == pointersByTarget_.end()) return;
    for (RuntimeObjectPointer* pointer : found->second) pointer->target_ = nullptr;
    pointersByTarget_.erase(found);
}

bool RuntimeTopology::wouldCreateCycle(const RuntimeObject* parent, IRuntimeObject* child) const {
    if (parent == child) {
        return true;
    }

    std::set<IRuntimeObject*> visited;
    std::vector<IRuntimeObject*> pending = {child};
    while (!pending.empty()) {
        IRuntimeObject* current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second) {
            continue;
        }
        if (current == parent) {
            return true;
        }

        const auto childrenFound = childrenByParent_.find(dynamic_cast<RuntimeObject*>(current));
        if (childrenFound == childrenByParent_.end()) {
            continue;
        }
        for (const auto& [name, descendant] : childrenFound->second) {
            static_cast<void>(name);
            pending.push_back(descendant);
        }
    }
    return false;
}

bool RuntimeTopology::connect(RuntimeObject* parent, std::string name, IRuntimeObject* child, bool overwrite) {
    std::map<std::string, IRuntimeObject*>& children = childrenByParent_[parent];
    const auto found = children.find(name);
    if (found != children.end() && !overwrite) {
        return false;
    }
    children[std::move(name)] = child;
    rebuildIncoming();
    return true;
}

bool RuntimeTopology::disconnect(RuntimeObject* parent, const std::string& name) {
    const auto parentFound = childrenByParent_.find(parent);
    if (parentFound == childrenByParent_.end() || parentFound->second.erase(name) == 0) {
        return false;
    }
    if (parentFound->second.empty()) {
        childrenByParent_.erase(parentFound);
    }
    rebuildIncoming();
    return true;
}

IRuntimeObject* RuntimeTopology::getChildItem(const RuntimeObject* parent, const std::string& name) const {
    const auto parentFound = childrenByParent_.find(const_cast<RuntimeObject*>(parent));
    if (parentFound == childrenByParent_.end()) {
        return nullptr;
    }
    const auto childFound = parentFound->second.find(name);
    return childFound == parentFound->second.end() ? nullptr : childFound->second;
}

RuntimeChildList RuntimeTopology::getChildren(const RuntimeObject* parent) const {
    RuntimeChildList children;
    const auto parentFound = childrenByParent_.find(const_cast<RuntimeObject*>(parent));
    if (parentFound == childrenByParent_.end()) {
        return children;
    }
    children.reserve(parentFound->second.size());
    for (const auto& [name, child] : parentFound->second) {
        children.push_back({name, child});
    }
    return children;
}

DetachedTopology RuntimeTopology::detachTopology(RuntimeObject* object) {
    DetachedTopology detached;
    const auto incomingFound = parentsByChild_.find(object);
    if (incomingFound != parentsByChild_.end()) {
        detached.incoming = incomingFound->second;
    }

    const auto outgoingFound = childrenByParent_.find(object);
    if (outgoingFound != childrenByParent_.end()) {
        for (const auto& [name, child] : outgoingFound->second) {
            detached.outgoing.push_back({object, name, child});
        }
        childrenByParent_.erase(outgoingFound);
    }

    for (const TopologyEdge& edge : detached.incoming) {
        const auto parentFound = childrenByParent_.find(edge.parent);
        if (parentFound == childrenByParent_.end()) {
            continue;
        }
        parentFound->second.erase(edge.name);
        if (parentFound->second.empty()) {
            childrenByParent_.erase(parentFound);
        }
    }
    rebuildIncoming();
    return detached;
}

RuntimeSubscription RuntimeTopology::observe(RuntimeObject* subscriber, IRuntimeObject* source,
                                             RuntimeEventTypeView type) {
    RuntimeObject* runtimeSource = dynamic_cast<RuntimeObject*>(source);
    if (type.empty() || subscriber == nullptr || runtimeSource == nullptr || !subscriber->isActive()
        || !runtimeSource->isActive() || subscriber->topology() != this
        || runtimeSource->topology() != this) {
        return RuntimeSubscription();
    }

    const RuntimeEventType eventType(type);
    const std::size_t id = nextSubscriptionId_++;
    subscriptionsById_.emplace(id, EventSubscription{subscriber, runtimeSource, eventType});
    subscriptionsBySubscriber_[subscriber].insert(id);
    subscriptionsBySource_[{runtimeSource, eventType}].insert(id);
    return detail::createRuntimeSubscription(this, id);
}

void RuntimeTopology::cancelSubscription(std::size_t id) noexcept {
    removeSubscription(id);
}

bool RuntimeTopology::isSubscriptionActive(std::size_t id) const noexcept {
    return id != 0 && subscriptionsById_.find(id) != subscriptionsById_.end();
}

void RuntimeTopology::publish(RuntimeObject* source, const RuntimeObjectEvent& event) {
    const auto sourceFound = subscriptionsBySource_.find({source, event.type});
    if (sourceFound == subscriptionsBySource_.end()) {
        return;
    }

    const std::vector<std::size_t> ids(sourceFound->second.begin(), sourceFound->second.end());
    for (const std::size_t id : ids) {
        const auto subscriptionFound = subscriptionsById_.find(id);
        if (subscriptionFound != subscriptionsById_.end()) {
            subscriptionFound->second.subscriber->deliverEvent(event);
        }
    }
}

void RuntimeTopology::rebuildIncoming() {
    parentsByChild_.clear();
    for (const auto& [parent, children] : childrenByParent_) {
        for (const auto& [name, child] : children) {
            parentsByChild_[child].push_back({parent, name, child});
        }
    }
}

void RuntimeTopology::removeSubscription(std::size_t id) noexcept {
    const auto found = subscriptionsById_.find(id);
    if (found == subscriptionsById_.end()) {
        return;
    }

    const EventSubscription subscription = found->second;
    subscriptionsById_.erase(found);
    const auto subscriberFound = subscriptionsBySubscriber_.find(subscription.subscriber);
    if (subscriberFound != subscriptionsBySubscriber_.end()) {
        subscriberFound->second.erase(id);
        if (subscriberFound->second.empty()) {
            subscriptionsBySubscriber_.erase(subscriberFound);
        }
    }
    const std::pair<RuntimeObject*, RuntimeEventType> sourceKey = {subscription.source, subscription.type};
    const auto sourceFound = subscriptionsBySource_.find(sourceKey);
    if (sourceFound != subscriptionsBySource_.end()) {
        sourceFound->second.erase(id);
        if (sourceFound->second.empty()) {
            subscriptionsBySource_.erase(sourceFound);
        }
    }
}

void RuntimeTopology::removeSubscriptionsFor(RuntimeObject* object) noexcept {
    std::set<std::size_t> ids;
    const auto subscriberFound = subscriptionsBySubscriber_.find(object);
    if (subscriberFound != subscriptionsBySubscriber_.end()) {
        ids.insert(subscriberFound->second.begin(), subscriberFound->second.end());
    }
    for (const auto& [sourceKey, sourceIds] : subscriptionsBySource_) {
        if (sourceKey.first == object) {
            ids.insert(sourceIds.begin(), sourceIds.end());
        }
    }
    for (const std::size_t id : ids) {
        removeSubscription(id);
    }
}

} // namespace

namespace detail {

RuntimeSubscription createRuntimeSubscription(void* control, std::size_t id) {
    return RuntimeSubscription(control, id);
}

void cancelRuntimeSubscription(void* control, std::size_t id) noexcept {
    if (control != nullptr) {
        static_cast<RuntimeTopology*>(control)->cancelSubscription(id);
    }
}

bool isRuntimeSubscriptionActive(void* control, std::size_t id) noexcept {
    return control != nullptr && static_cast<RuntimeTopology*>(control)->isSubscriptionActive(id);
}

IRuntimeObject* createRuntimeObject(RuntimeObjectBridge bridge) {
    return new RuntimeObject(std::move(bridge), runtimeTopology());
}

IRuntimeObjectPointer* createRuntimeObjectPointer(IRuntimeObject* initialObject) {
    auto* pointer = new RuntimeObjectPointer(runtimeTopology());
    if (initialObject != nullptr) pointer->Bind(initialObject);
    return pointer;
}

} // namespace detail

IRuntimeObject* Runtime::make() {
    return detail::createRuntimeObject({});
}

IRuntimeObjectPointer* Runtime::makePointer() {
    return detail::createRuntimeObjectPointer(nullptr);
}

IRuntimeObjectPointer* Runtime::makePointer(IRuntimeObject* object) {
    return detail::createRuntimeObjectPointer(object);
}

} // namespace iobject
