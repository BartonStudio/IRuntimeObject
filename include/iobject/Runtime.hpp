#pragma once

#include "IRuntimeObject.hpp"
#include "IRuntimeObjectPointer.hpp"

#include <concepts>
#include <functional>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace iobject::detail {

struct TypeConverter {
    std::function<void*(void*)> mutableQuery;
    std::function<const void*(const void*)> constQuery;
};

struct TypeDescription {
    std::unordered_map<std::type_index, TypeConverter> converters;
};

/// Minimal ABI bridge for the static-library factory; callers must not depend on it.
struct RuntimeObjectBridge {
    std::shared_ptr<void> lifetime;
    void* object = nullptr;
    const TypeDescription* types = nullptr;
    std::function<bool(const void*, DataChannelView, DataReceiver)> readData;
    std::function<bool(void*, DataChannelView, ByteInput)> writeData;
};

/// Static-library factory; callers should create nodes through Runtime.
IRuntimeObject* createRuntimeObject(RuntimeObjectBridge bridge);
IRuntimeObjectPointer* createRuntimeObjectPointer(IRuntimeObject* initialObject);

} // namespace iobject::detail

namespace iobject {

template<typename Source>
class TypeBuilder final {
public:
    explicit TypeBuilder(detail::TypeDescription& description) noexcept : description_(description) {}

    /// 登记 Source* 可直接转换为 Target* 的公开类型。
    template<typename Target>
    void As() {
        static_assert(std::is_convertible_v<Source*, Target*>,
                      "TypeBuilder::As<Target>() requires Source* convertible to Target*.");
        static_assert(std::is_convertible_v<const Source*, const Target*>,
                      "TypeBuilder::As<Target>() requires const Source* convertible to const Target*.");
        Add<Target>(
            [](Source& object) -> Target* { return static_cast<Target*>(std::addressof(object)); },
            [](const Source& object) -> const Target* {
                return static_cast<const Target*>(std::addressof(object));
            });
    }

    /// 登记可经由稳定成员地址取得的公开类型。
    template<typename Target, typename Member>
    void As(Member Source::*member) {
        static_assert(std::is_convertible_v<Member*, Target*>,
                      "TypeBuilder::As<Target>(member) requires the member address convertible to Target*.");
        static_assert(std::is_convertible_v<const Member*, const Target*>,
                      "TypeBuilder::As<Target>(member) requires the const member address convertible to const Target*.");
        Add<Target>(
            [member](Source& object) -> Target* {
                return static_cast<Target*>(std::addressof(object.*member));
            },
            [member](const Source& object) -> const Target* {
                return static_cast<const Target*>(std::addressof(object.*member));
            });
    }

    /// 登记由只读业务逻辑取得的公开类型；回调可按对象状态返回 nullptr。
    template<typename Target, typename Callable>
        requires std::invocable<Callable&, Source&>
              && std::invocable<const Callable&, const Source&>
              && std::convertible_to<std::invoke_result_t<Callable&, Source&>, Target*>
              && std::convertible_to<std::invoke_result_t<const Callable&, const Source&>, const Target*>
    void As(Callable callable) {
        Add<Target>(
            [callable](Source& object) -> Target* {
                return std::invoke(callable, object);
            },
            [callable](const Source& object) -> const Target* {
                return std::invoke(callable, object);
            });
    }

private:
    template<typename Target, typename MutableCallable, typename ConstCallable>
    void Add(MutableCallable mutableCallable, ConstCallable constCallable) {
        static_assert(!std::is_void_v<Target>, "TypeBuilder cannot register void.");
        const std::type_index targetType(typeid(Target));
        if (description_.converters.contains(targetType)) {
            throw std::logic_error("同一个目标类型只能登记一条 As 转换规则。");
        }
        description_.converters.emplace(targetType, detail::TypeConverter{
            [converter = std::move(mutableCallable)](void* object) -> void* {
                return converter(*static_cast<Source*>(object));
            },
            [converter = std::move(constCallable)](const void* object) -> const void* {
                return converter(*static_cast<const Source*>(object));
            }});
    }

    detail::TypeDescription& description_;
};

namespace detail {

template<typename T>
const TypeDescription& typeDescription() {
    static const TypeDescription description = [] {
        TypeDescription result;
        TypeBuilder<T> builder(result);
        builder.template As<T>();
        if constexpr (requires(TypeBuilder<T>& types) { T::RegisterTypes(types); }) {
            T::RegisterTypes(builder);
        }
        return result;
    }();
    return description;
}

template<typename T>
RuntimeObjectBridge makeBridge(std::shared_ptr<void> lifetime, T* object) {
    RuntimeObjectBridge bridge;
    bridge.lifetime = std::move(lifetime);
    bridge.object = object;
    bridge.types = std::addressof(typeDescription<T>());
    if constexpr (requires(const T& value, DataChannelView channel, DataReceiver receiver) {
        { value.ReadData(channel, std::move(receiver)) } -> std::convertible_to<bool>;
    }) {
        bridge.readData = [](const void* value, DataChannelView channel, DataReceiver receiver) {
            return static_cast<const T*>(value)->ReadData(channel, std::move(receiver));
        };
    }
    if constexpr (requires(T& value, DataChannelView channel, ByteInput data) {
        { value.WriteData(channel, data) } -> std::convertible_to<bool>;
    }) {
        bridge.writeData = [](void* value, DataChannelView channel, ByteInput data) {
            return static_cast<T*>(value)->WriteData(channel, data);
        };
    }
    return bridge;
}

} // namespace detail

/// 运行时节点创建门面。
class Runtime final {
public:
    Runtime() = delete;

    /// 创建不承载原生对象的纯运行时节点；调用方负责 delete 返回节点。
    static IRuntimeObject* make();
    /// 创建未绑定的透明指针节点；调用方负责 delete 返回节点。
    static IRuntimeObjectPointer* makePointer();
    /// 创建并立即尝试绑定 object 的透明指针节点；绑定失败时仍返回未绑定节点。
    static IRuntimeObjectPointer* makePointer(IRuntimeObject* object);

    /// 创建并按共享所有权承载一个新建的原生对象；调用方负责 delete 返回节点。
    template<typename T, typename... Args>
    static IRuntimeObject* make(Args&&... args) {
        std::shared_ptr<T> value = std::make_shared<T>(std::forward<Args>(args)...);
        return detail::createRuntimeObject(detail::makeBridge<T>(std::shared_ptr<void>(value), value.get()));
    }

    /// 创建借用 obj 的运行时节点；调用方负责 obj 的生命周期及 delete 返回节点。
    template<typename T>
    static IRuntimeObject* ref(T& obj) {
        return detail::createRuntimeObject(detail::makeBridge<T>(
            std::shared_ptr<void>(std::addressof(obj), [](void*) noexcept {}), std::addressof(obj)));
    }

    /// 创建共享持有 ptr 的运行时节点；调用方负责 delete 返回节点。
    template<typename T>
    static IRuntimeObject* share(std::shared_ptr<T> ptr) {
        if (!ptr) {
            return nullptr;
        }
        T* object = ptr.get();
        return detail::createRuntimeObject(
            detail::makeBridge<T>(std::shared_ptr<void>(std::move(ptr)), object));
    }

    /// 创建包装裸指针的节点；owned 为 true 时节点 delete 会 delete ptr。
    template<typename T>
    static IRuntimeObject* fromPtr(T* ptr, bool owned = false) {
        if (ptr == nullptr) {
            return nullptr;
        }
        if (owned) {
            return detail::createRuntimeObject(detail::makeBridge<T>(
                std::shared_ptr<void>(ptr, [](void* value) noexcept { delete static_cast<T*>(value); }), ptr));
        }
        return detail::createRuntimeObject(detail::makeBridge<T>(
            std::shared_ptr<void>(ptr, [](void*) noexcept {}), ptr));
    }
};

} // namespace iobject
