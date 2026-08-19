# RuntimeDomain 与 RuntimeBridgeRoot 第一版实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现规格书 `docs/superpowers/specs/2026-08-18-runtime-domain-bridge-root-design.md` 的 C++ 侧模型：`RuntimeDomain`（持有根锚点）、`RuntimeBridgeRoot`（会话入口）、`RuntimeSession`（句柄表 + 中继节点订阅），不引入传输与协议。

**Architecture:** 单默认域复用现有进程内全局 `RuntimeTopology`；`RuntimeDomain` 构造时 `Runtime::make()` 创建并持有纯运行时根锚点；每个 `RuntimeSession` 持有一个 `Runtime::make()` 中继节点作为所有远程订阅的订阅者（满足"订阅者必须是 IRuntimeObject"的既有规则），并维护会话内不透明 `RemoteObjectHandle` 映射。未来传输层把协议消息转发到 `RuntimeSession` 方法即可，本计划不实现传输。

**Tech Stack:** C++20、CMake（静态库 `IObject`）、CTest + 轻量断言宏（项目原无自动化测试设施，本计划新增 `tests/`）。

**范围说明（与规格书一致）：** 不实现 WebSocket/Socket 等传输、不实现消息序列化协议、不允许远程改拓扑或释放对象、不提供远程 `As<T>`。第一版事件消息不传输通用载荷，仅在载荷可 `As<DataChannelChangedEventData>()` 时填充 `channel` 字段（保证 `DataChannelChanged` 远程可用）；其余载荷语义留待协议设计。

**构建与测试命令（Windows / MSVC，全程复用现有 `build/` 目录）：**

```bash
cmake -S . -B build                      # 新增 tests/ 后需要重新配置
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

---

### Task 1: 测试设施（tests/ + CTest + 断言宏）

**Files:**
- Create: `tests/TestCheck.hpp`
- Create: `tests/SmokeTest.cpp`
- Create: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`（在 `option(BUILD_EXAMPLES ...)` 块之后追加 tests 块）

- [ ] **Step 1: 编写断言宏头文件**

`tests/TestCheck.hpp`：

```cpp
#pragma once

#include <cstdio>
#include <cstdlib>

// 轻量测试断言：失败时输出位置并以非零码退出，供 CTest 判定。
#define TEST_CHECK(condition)                                                                  \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            std::fprintf(stderr, "TEST_CHECK 失败 %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            std::exit(1);                                                                      \
        }                                                                                      \
    } while (0)
```

- [ ] **Step 2: 编写冒烟测试**

`tests/SmokeTest.cpp`：

```cpp
#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>

int main() {
    iobject::IRuntimeObject* node = iobject::Runtime::make();
    TEST_CHECK(node != nullptr);
    delete node;
    return 0;
}
```

- [ ] **Step 3: 编写 tests/CMakeLists.txt**

```cmake
add_executable(test_smoke SmokeTest.cpp)
target_link_libraries(test_smoke PRIVATE IObject::IObject)
add_test(NAME smoke COMMAND test_smoke)
```

- [ ] **Step 4: 修改顶层 CMakeLists.txt**

在 `CMakeLists.txt` 的 `if(BUILD_EXAMPLES) ... endif()` 块之后追加：

```cmake
option(BUILD_TESTS "Build automated tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

- [ ] **Step 5: 重新配置并运行，验证测试通过**

```bash
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

预期：`100% tests passed, 0 tests failed`，包含 `smoke`。

- [ ] **Step 6: Commit**

```bash
git add tests/TestCheck.hpp tests/SmokeTest.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "test: add ctest harness with lightweight assertion macro"
```

---

### Task 2: RuntimeDomain（域与根锚点）

**Files:**
- Create: `include/iobject/RuntimeDomain.hpp`
- Create: `src/RuntimeDomain.cpp`
- Modify: `CMakeLists.txt`（库源文件与安装头列表）
- Create: `tests/RuntimeDomainTest.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 编写失败测试**

`tests/RuntimeDomainTest.cpp`：

```cpp
#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeDomain.hpp>

int main() {
    {
        iobject::RuntimeDomain domain;
        iobject::IRuntimeObject* anchor = domain.RootAnchor();
        TEST_CHECK(anchor != nullptr);
        TEST_CHECK(&domain.BridgeRoot() != nullptr);

        // 业务对象接入根锚点子树后可沿拓扑查询。
        iobject::IRuntimeObject* child = iobject::Runtime::make();
        TEST_CHECK(anchor->Connect("Player", child));
        TEST_CHECK(anchor->GetChildItem("Player") == child);
        delete child;
    }
    // 域析构后进程正常退出即说明根锚点销毁无误。
    return 0;
}
```

`tests/CMakeLists.txt` 追加：

```cmake
add_executable(test_runtime_domain RuntimeDomainTest.cpp)
target_link_libraries(test_runtime_domain PRIVATE IObject::IObject)
add_test(NAME runtime_domain COMMAND test_runtime_domain)
```

- [ ] **Step 2: 运行测试验证失败**

```bash
cmake -S . -B build && cmake --build build --config Debug
```

预期：编译失败，`iobject/RuntimeDomain.hpp` 不存在。

- [ ] **Step 3: 实现 RuntimeDomain**

`include/iobject/RuntimeDomain.hpp`：

```cpp
#pragma once

#include <memory>

namespace iobject {

class IRuntimeObject;
class RuntimeBridgeRoot;

/// 运行时域：第一版对应进程内默认全局拓扑，自动创建并持有唯一根锚点与桥接入口。
/// 一个活动 IRuntimeObject 同一时刻有且仅属于一个域，生命周期内不迁移。
/// 硬约束（约定，不加运行时分支）：域及桥接服务存活期间，不得对根锚点 Release 或 delete。
/// 正常销毁顺序：先关闭全部 RuntimeSession，再销毁业务对象，最后销毁 RuntimeDomain。
class RuntimeDomain final {
public:
    RuntimeDomain();
    ~RuntimeDomain();

    RuntimeDomain(const RuntimeDomain&) = delete;
    RuntimeDomain& operator=(const RuntimeDomain&) = delete;

    /// 域持有的纯运行时根锚点；业务方用 Connect 把业务对象接入其子树。
    IRuntimeObject* RootAnchor() const noexcept;
    /// 域内唯一桥接入口；所有远程会话经它创建。
    RuntimeBridgeRoot& BridgeRoot() const noexcept;

private:
    IRuntimeObject* rootAnchor_ = nullptr;
    std::unique_ptr<RuntimeBridgeRoot> bridgeRoot_;
};

} // namespace iobject
```

`src/RuntimeDomain.cpp`：

```cpp
#include <iobject/RuntimeDomain.hpp>

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>

namespace iobject {

RuntimeDomain::RuntimeDomain()
    : rootAnchor_(Runtime::make()),
      bridgeRoot_(std::make_unique<RuntimeBridgeRoot>(rootAnchor_)) {}

RuntimeDomain::~RuntimeDomain() {
    bridgeRoot_.reset();  // 先停桥接入口。
    delete rootAnchor_;   // 再销毁根锚点（析构自动执行释放流程）。
}

IRuntimeObject* RuntimeDomain::RootAnchor() const noexcept {
    return rootAnchor_;
}

RuntimeBridgeRoot& RuntimeDomain::BridgeRoot() const noexcept {
    return *bridgeRoot_;
}

} // namespace iobject
```

修改 `CMakeLists.txt`：库源文件列表改为

```cmake
add_library(IObject STATIC
    src/RuntimeObject.cpp
    src/RuntimeDomain.cpp
    src/RuntimeBridge.cpp
)
```

安装头列表改为

```cmake
install(FILES
    include/iobject/IRuntimeObject.hpp
    include/iobject/IRuntimeObjectPointer.hpp
    include/iobject/Runtime.hpp
    include/iobject/RuntimeDomain.hpp
    include/iobject/RuntimeBridge.hpp
    DESTINATION include/iobject
)
```

注意：`src/RuntimeBridge.cpp` 与 `include/iobject/RuntimeBridge.hpp` 在 Task 3 创建；本任务先创建占位文件使构建通过——`include/iobject/RuntimeBridge.hpp`：

```cpp
#pragma once

namespace iobject {

class IRuntimeObject;

/// 域内唯一桥接入口；所有远程会话经它创建。完整定义见 Task 3。
class RuntimeBridgeRoot final {
public:
    explicit RuntimeBridgeRoot(IRuntimeObject* rootAnchor) noexcept : rootAnchor_(rootAnchor) {}

    RuntimeBridgeRoot(const RuntimeBridgeRoot&) = delete;
    RuntimeBridgeRoot& operator=(const RuntimeBridgeRoot&) = delete;

private:
    IRuntimeObject* rootAnchor_;
};

} // namespace iobject
```

`src/RuntimeBridge.cpp`：

```cpp
#include <iobject/RuntimeBridge.hpp>
```

- [ ] **Step 4: 运行测试验证通过**

```bash
cmake -S . -B build && cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

预期：`runtime_domain` 与 `smoke` 全部通过。

- [ ] **Step 5: Commit**

```bash
git add include/iobject/RuntimeDomain.hpp src/RuntimeDomain.cpp include/iobject/RuntimeBridge.hpp src/RuntimeBridge.cpp CMakeLists.txt tests/RuntimeDomainTest.cpp tests/CMakeLists.txt
git commit -m "feat: add RuntimeDomain with owned root anchor"
```

---

### Task 3: RuntimeSession 句柄表与对象解析

**Files:**
- Modify: `include/iobject/RuntimeBridge.hpp`（替换 Task 2 占位为完整定义）
- Modify: `src/RuntimeBridge.cpp`（实现句柄表与解析）
- Create: `tests/RuntimeSessionResolveTest.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 编写失败测试**

`tests/RuntimeSessionResolveTest.cpp`：

```cpp
#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeDomain.hpp>

int main() {
    iobject::RuntimeDomain domain;
    iobject::IRuntimeObject* player = iobject::Runtime::make();
    iobject::IRuntimeObject* decoder = iobject::Runtime::make();
    TEST_CHECK(domain.RootAnchor()->Connect("Player", player));
    TEST_CHECK(player->Connect("Decoder", decoder));

    std::unique_ptr<iobject::RuntimeSession> session = domain.BridgeRoot().OpenSession();
    TEST_CHECK(session != nullptr);
    TEST_CHECK(session->IsOpen());

    // 单层与多级路径解析。
    const iobject::RemoteObjectHandle playerHandle = session->ResolveRootChild("Player");
    TEST_CHECK(playerHandle != 0);
    const iobject::RemoteObjectHandle decoderHandle = session->ResolveRootChild("Player.Decoder");
    TEST_CHECK(decoderHandle != 0);
    TEST_CHECK(decoderHandle != playerHandle);

    // 同一对象经不同路径到达返回同一句柄。
    const iobject::RemoteObjectHandle decoderAgain = session->ResolveChild(playerHandle, "Decoder");
    TEST_CHECK(decoderAgain == decoderHandle);

    // 未命中路径与无效句柄返回 0。
    TEST_CHECK(session->ResolveRootChild("Missing") == 0);
    TEST_CHECK(session->ResolveRootChild("Player.Missing") == 0);
    TEST_CHECK(session->ResolveRootChild("") == 0);
    TEST_CHECK(session->ResolveChild(9999, "Decoder") == 0);

    // 不同会话的句柄互相独立。
    std::unique_ptr<iobject::RuntimeSession> other = domain.BridgeRoot().OpenSession();
    TEST_CHECK(other != nullptr);
    TEST_CHECK(other->ResolveChild(playerHandle, "Decoder") == 0);

    // 未接入根锚点子树的域内对象远程不可见。
    iobject::IRuntimeObject* detached = iobject::Runtime::make();
    static_cast<void>(detached);  // 无句柄可指向它：无法经任何解析路径到达。

    session->Close();
    TEST_CHECK(!session->IsOpen());
    TEST_CHECK(session->ResolveRootChild("Player") == 0);

    delete detached;
    delete decoder;
    delete player;
    return 0;
}
```

`tests/CMakeLists.txt` 追加：

```cmake
add_executable(test_runtime_session_resolve RuntimeSessionResolveTest.cpp)
target_link_libraries(test_runtime_session_resolve PRIVATE IObject::IObject)
add_test(NAME runtime_session_resolve COMMAND test_runtime_session_resolve)
```

- [ ] **Step 2: 运行测试验证失败**

```bash
cmake -S . -B build && cmake --build build --config Debug
```

预期：编译失败（`RuntimeSession`、`RemoteObjectHandle`、`OpenSession` 未定义）。

- [ ] **Step 3: 实现完整 RuntimeBridge 头与句柄解析**

用以下内容**整体替换** `include/iobject/RuntimeBridge.hpp`：

```cpp
#pragma once

#include "IRuntimeObject.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace iobject {

/// 会话内不透明远程对象句柄；0 表示无效。不暴露内存地址，跨会话不可混用。
using RemoteObjectHandle = std::uint64_t;

/// 推送给远程端的事件消息。
/// 第一版不传输通用事件载荷；仅当载荷可 As<DataChannelChangedEventData>() 时填充 channel。
struct RemoteEventMessage {
    RemoteObjectHandle source = 0;
    RuntimeEventType type;
    DataChannel channel;
};

using RemoteEventCallback = std::function<void(const RemoteEventMessage&)>;

/// 一个远程连接对应一个会话；方法逐一对应 JS 端接口，未来传输层把协议消息转发到这里。
/// 会话不是线程安全的：与框架其余部分一样假定单线程事件循环。
class RuntimeSession final {
public:
    ~RuntimeSession();

    RuntimeSession(const RuntimeSession&) = delete;
    RuntimeSession& operator=(const RuntimeSession&) = delete;

    /// 对应 JS runtime.Root.GetChildItem(path)；未命中返回 0。
    RemoteObjectHandle ResolveRootChild(const std::string& path);
    /// 对应 JS obj.GetChildItem(path)；handle 无效或路径未命中返回 0。
    RemoteObjectHandle ResolveChild(RemoteObjectHandle handle, const std::string& path);

    bool ReadData(RemoteObjectHandle handle, DataChannelView channel, DataReceiver receiver) const;
    bool WriteData(RemoteObjectHandle handle, DataChannelView channel, ByteInput data);

    /// 对应 JS obj.SubscribeEvent(type, handler)；返回会话内订阅 ID，0 表示失败。
    /// 回调在 C++ 事件同步派发期间执行，不得向框架抛出异常。
    std::uint64_t SubscribeEvent(RemoteObjectHandle handle, RuntimeEventTypeView type,
                                 RemoteEventCallback callback);
    void CancelEvent(std::uint64_t subscriptionId) noexcept;

    /// 关闭会话：全部句柄与订阅立即失效；幂等。
    void Close() noexcept;
    bool IsOpen() const noexcept;

private:
    friend class RuntimeBridgeRoot;
    RuntimeSession(IRuntimeObject* rootAnchor, IRuntimeObject* relay);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// 域内唯一桥接入口；不拥有根锚点（根锚点由 RuntimeDomain 持有）。
class RuntimeBridgeRoot final {
public:
    explicit RuntimeBridgeRoot(IRuntimeObject* rootAnchor) noexcept : rootAnchor_(rootAnchor) {}

    RuntimeBridgeRoot(const RuntimeBridgeRoot&) = delete;
    RuntimeBridgeRoot& operator=(const RuntimeBridgeRoot&) = delete;

    /// 根锚点不可用时返回 nullptr。会话由调用方持有，须先于 RuntimeDomain 销毁。
    std::unique_ptr<RuntimeSession> OpenSession();

private:
    IRuntimeObject* rootAnchor_;
};

} // namespace iobject
```

用以下内容**整体替换** `src/RuntimeBridge.cpp`：

```cpp
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

        const RemoteObjectHandle handle = nextHandle++;
        handlesByObject.emplace(object, handle);
        objectsByHandle.emplace(handle, object);
        // 监视 Released：对象退出 IRuntimeObject 系统后句柄立即失效。
        releaseWatchByObject.emplace(
            object, relay->SubscribeEvent(object, RuntimeEventTypes::Released,
                                          [this, object](const RuntimeObjectEvent&) {
                                              invalidate(object);
                                          }));
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
        for (auto current = eventSubscriptions.begin(); current != eventSubscriptions.end();) {
            if (current->second.source == object) {
                current = eventSubscriptions.erase(current);
            } else {
                ++current;
            }
        }
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
}

RuntimeSession::~RuntimeSession() {
    impl_->close();
}

RemoteObjectHandle RuntimeSession::ResolveRootChild(const std::string& path) {
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

std::unique_ptr<RuntimeSession> RuntimeBridgeRoot::OpenSession() {
    if (rootAnchor_ == nullptr) {
        return nullptr;
    }
    IRuntimeObject* relay = Runtime::make();
    return std::unique_ptr<RuntimeSession>(new RuntimeSession(rootAnchor_, relay));
}

} // namespace iobject
```

- [ ] **Step 4: 运行测试验证通过**

```bash
cmake -S . -B build && cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

预期：`runtime_session_resolve` 等全部测试通过。

- [ ] **Step 5: Commit**

```bash
git add include/iobject/RuntimeBridge.hpp src/RuntimeBridge.cpp tests/RuntimeSessionResolveTest.cpp tests/CMakeLists.txt
git commit -m "feat: add RuntimeSession handle table and path resolution"
```

---

### Task 4: 会话数据通道读写

**Files:**
- Create: `tests/RuntimeSessionDataTest.cpp`
- Modify: `tests/CMakeLists.txt`

实现已在 Task 3 完成（`RuntimeSession::ReadData` / `WriteData`），本任务补齐行为测试。

- [ ] **Step 1: 编写测试**

`tests/RuntimeSessionDataTest.cpp`：

```cpp
#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <array>
#include <cstdint>

namespace {

class Device final {
public:
    bool ReadData(iobject::DataChannelView channel, iobject::DataReceiver receiver) const {
        if (channel != "State") {
            return false;
        }
        const std::array<std::uint8_t, 4> bytes{
            static_cast<std::uint8_t>(state_ & 0xFF),
            static_cast<std::uint8_t>((state_ >> 8) & 0xFF),
            static_cast<std::uint8_t>((state_ >> 16) & 0xFF),
            static_cast<std::uint8_t>((state_ >> 24) & 0xFF)};
        receiver(bytes);
        return true;
    }

    bool WriteData(iobject::DataChannelView channel, iobject::ByteInput data) {
        if (channel != "State" || data.size() != 4) {
            return false;
        }
        state_ = static_cast<std::uint32_t>(data[0])
               | (static_cast<std::uint32_t>(data[1]) << 8)
               | (static_cast<std::uint32_t>(data[2]) << 16)
               | (static_cast<std::uint32_t>(data[3]) << 24);
        return true;
    }

private:
    std::uint32_t state_ = 0;
};

} // namespace

int main() {
    iobject::RuntimeDomain domain;
    iobject::IRuntimeObject* device = iobject::Runtime::make<Device>();
    TEST_CHECK(domain.RootAnchor()->Connect("Device", device));

    std::unique_ptr<iobject::RuntimeSession> session = domain.BridgeRoot().OpenSession();
    const iobject::RemoteObjectHandle handle = session->ResolveRootChild("Device");
    TEST_CHECK(handle != 0);

    // 经会话写入，再经会话读回。
    const std::array<std::uint8_t, 4> input{0x2C, 0x01, 0x00, 0x00};  // 300
    TEST_CHECK(session->WriteData(handle, "State", input));
    std::uint32_t readBack = 0;
    std::size_t receiverCalls = 0;
    TEST_CHECK(session->ReadData(handle, "State", [&](iobject::ByteView bytes) {
        ++receiverCalls;
        TEST_CHECK(bytes.size() == 4);
        readBack = static_cast<std::uint32_t>(bytes[0])
                 | (static_cast<std::uint32_t>(bytes[1]) << 8)
                 | (static_cast<std::uint32_t>(bytes[2]) << 16)
                 | (static_cast<std::uint32_t>(bytes[3]) << 24);
    }));
    TEST_CHECK(receiverCalls == 1);
    TEST_CHECK(readBack == 300);

    // 未知通道、无效句柄均失败。
    TEST_CHECK(!session->ReadData(handle, "Missing", [](iobject::ByteView) {}));
    TEST_CHECK(!session->WriteData(9999, "State", input));

    session->Close();
    TEST_CHECK(!session->WriteData(handle, "State", input));

    delete device;
    return 0;
}
```

`tests/CMakeLists.txt` 追加：

```cmake
add_executable(test_runtime_session_data RuntimeSessionDataTest.cpp)
target_link_libraries(test_runtime_session_data PRIVATE IObject::IObject)
add_test(NAME runtime_session_data COMMAND test_runtime_session_data)
```

- [ ] **Step 2: 运行测试验证通过**

```bash
cmake -S . -B build && cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

预期：`runtime_session_data` 通过。

- [ ] **Step 3: Commit**

```bash
git add tests/RuntimeSessionDataTest.cpp tests/CMakeLists.txt
git commit -m "test: cover session data channel read and write"
```

---

### Task 5: 会话事件订阅转发与句柄失效

**Files:**
- Create: `tests/RuntimeSessionEventTest.cpp`
- Modify: `tests/CMakeLists.txt`

实现已在 Task 3 完成（`SubscribeEvent` / `CancelEvent` / `invalidate`），本任务补齐行为测试。

- [ ] **Step 1: 编写测试**

`tests/RuntimeSessionEventTest.cpp`：

```cpp
#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeDomain.hpp>

namespace {

class Counter final {
public:
    void Increase() {
        ++value_;
        PublishChanged();
    }

    bool ReadData(iobject::DataChannelView channel, iobject::DataReceiver receiver) const {
        if (channel != "Value") {
            return false;
        }
        const std::uint8_t bytes[1] = {static_cast<std::uint8_t>(value_)};
        receiver(iobject::ByteView(bytes, 1));
        return true;
    }

    void PublishChanged() {
        node_->Publish(iobject::RuntimeEventTypes::DataChannelChanged,
                       iobject::Runtime::make<iobject::DataChannelChangedEventData>("Value"),
                       true);
    }

    void Attach(iobject::IRuntimeObject* node) {
        node_ = node;
    }

private:
    iobject::IRuntimeObject* node_ = nullptr;
    int value_ = 0;
};

} // namespace

int main() {
    iobject::RuntimeDomain domain;
    auto* counter = new Counter();
    iobject::IRuntimeObject* node = iobject::Runtime::fromPtr(counter, true);
    counter->Attach(node);
    TEST_CHECK(domain.RootAnchor()->Connect("Counter", node));

    std::unique_ptr<iobject::RuntimeSession> session = domain.BridgeRoot().OpenSession();
    const iobject::RemoteObjectHandle handle = session->ResolveRootChild("Counter");
    TEST_CHECK(handle != 0);

    // 订阅 DataChannelChanged：转发 source 句柄、类型与 channel。
    int eventCount = 0;
    iobject::RemoteObjectHandle lastSource = 0;
    iobject::DataChannel lastChannel;
    const std::uint64_t subscription = session->SubscribeEvent(
        handle, iobject::RuntimeEventTypes::DataChannelChanged,
        [&](const iobject::RemoteEventMessage& message) {
            ++eventCount;
            lastSource = message.source;
            lastChannel = message.channel;
        });
    TEST_CHECK(subscription != 0);

    counter->Increase();
    TEST_CHECK(eventCount == 1);
    TEST_CHECK(lastSource == handle);
    TEST_CHECK(lastChannel == "Value");

    // CancelEvent 后不再收到。
    session->CancelEvent(subscription);
    counter->Increase();
    TEST_CHECK(eventCount == 1);

    // 对象 Release 后句柄失效，事件订阅随之解除。
    const std::uint64_t again = session->SubscribeEvent(
        handle, iobject::RuntimeEventTypes::DataChannelChanged,
        [&](const iobject::RemoteEventMessage&) { ++eventCount; });
    TEST_CHECK(again != 0);
    node->Release();
    TEST_CHECK(session->ResolveChild(handle, "Anything") == 0);
    TEST_CHECK(session->ReadData(handle, "Value", [](iobject::ByteView) {}) == false);
    counter->Increase();  // 节点已 Release，Publish 不再投递，也不应崩溃。
    TEST_CHECK(eventCount == 1);

    session->Close();
    delete node;  // owned=true，随节点 delete 释放 counter。
    return 0;
}
```

`tests/CMakeLists.txt` 追加：

```cmake
add_executable(test_runtime_session_event RuntimeSessionEventTest.cpp)
target_link_libraries(test_runtime_session_event PRIVATE IObject::IObject)
add_test(NAME runtime_session_event COMMAND test_runtime_session_event)
```

- [ ] **Step 2: 运行测试验证通过**

```bash
cmake -S . -B build && cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

预期：`runtime_session_event` 通过。

- [ ] **Step 3: Commit**

```bash
git add tests/RuntimeSessionEventTest.cpp tests/CMakeLists.txt
git commit -m "test: cover session event forwarding and handle invalidation"
```

---

### Task 6: 示例程序 09_RemoteBridgeTest

**Files:**
- Create: `example/09_RemoteBridgeTest.cpp`
- Modify: `example/CMakeLists.txt`

示例遵循既有示例风格（`printf` 演示用法，不用断言），展示域、根锚点、会话解析、读写与事件订阅的完整用法。

- [ ] **Step 1: 编写示例**

`example/09_RemoteBridgeTest.cpp`：

```cpp
// 09_RemoteBridgeTest：演示 RuntimeDomain / RuntimeBridgeRoot / RuntimeSession 的用法。
// 会话方法逐一对应未来 JS 端接口；本示例在进程内直接调用，不涉及真实传输。
#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <array>
#include <cstdint>
#include <cstdio>

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
```

`example/CMakeLists.txt` 追加：

```cmake
add_executable(test_09_remote_bridge 09_RemoteBridgeTest.cpp)
target_link_libraries(test_09_remote_bridge PRIVATE IObject::IObject)
```

- [ ] **Step 2: 构建并运行示例**

```bash
cmake -S . -B build && cmake --build build --config Debug
./build/example/Debug/test_09_remote_bridge.exe
```

预期：按序号输出 1–6，事件行显示 `type=DataChannelChanged channel=State`，`IsOpen=0`。

- [ ] **Step 3: Commit**

```bash
git add example/09_RemoteBridgeTest.cpp example/CMakeLists.txt
git commit -m "docs: add remote bridge usage example"
```

---

### Task 7: 文档更新与最终验证

**Files:**
- Modify: `IObject_规则书与设计评估.md`
- Modify: `IObject_后续关键能力清单.md`

- [ ] **Step 1: 更新规则书**

在 `IObject_规则书与设计评估.md` 第 1 节公开头列表追加两项（保持原有风格）：

```markdown
- `<iobject/RuntimeDomain.hpp>`：运行时域契约，自动持有唯一根锚点与桥接入口；
- `<iobject/RuntimeBridge.hpp>`：`RuntimeBridgeRoot` 与 `RuntimeSession` 远程桥接模型（不含传输与协议）。
```

并把"安装内容仅包括静态库、上述两个头文件"改为"上述五个头文件"。

在第 6 节"当前拓扑实例与多域演进"末尾追加一段：

```markdown
`RuntimeDomain` 已实现第一版：它对应当前单一全局拓扑，构造时自动创建并持有纯运行时根锚点（`RootAnchor()`）与唯一 `RuntimeBridgeRoot`（`BridgeRoot()`）。根锚点在域及桥接服务存活期间不得 `Release` 或 `delete`（约定，不加运行时分支）。销毁顺序：先关闭全部 `RuntimeSession`，再销毁业务对象，最后销毁域。`RuntimeSession` 由 `RuntimeBridgeRoot::OpenSession()` 创建，方法逐一对应 JS 端接口（`ResolveRootChild`/`ResolveChild` ↔ `GetChildItem`，`ReadData`/`WriteData`，`SubscribeEvent`/`CancelEvent`，`Close`）；远程可见范围是从根锚点沿 `Connect` 向下可达的子树，远程不能 `Connect`/`Disconnect`/`Release`/`As<T>`。会话为每个被引用对象分配会话内不透明 `RemoteObjectHandle`（不暴露内存地址，跨会话独立），同一对象经多条路径到达返回同一句柄；对象 `Release` 或析构后句柄立即失效。会话经一个私有中继节点登记全部远程订阅（满足订阅者必须是 `IRuntimeObject` 的既有规则），事件消息第一版不传输通用载荷，仅在载荷可 `As<DataChannelChangedEventData>()` 时携带 `channel`。传输层与消息协议未实现，未来只需把协议消息转发到 `RuntimeSession` 方法。
```

- [ ] **Step 2: 更新关键能力清单**

在 `IObject_后续关键能力清单.md` 的"多运行时域与跨域引用（未实现）"一节开头追加：

```markdown
`RuntimeDomain`、`RuntimeBridgeRoot` 与 `RuntimeSession` 的第一版已实现（仅 C++ 侧模型，单默认域，无传输与协议）：域自动持有根锚点，远程经会话句柄沿根锚点子树发现对象、读写通道并订阅事件。`RuntimeDomainManager`、`DomainId`、域内稳定对象 ID 索引与 `ExternalObjectRef` 仍未实现。
```

- [ ] **Step 3: 全量最终验证**

```bash
cmake -S . -B build && cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
./build/example/Debug/test_09_remote_bridge.exe
```

预期：全部测试通过（smoke、runtime_domain、runtime_session_resolve、runtime_session_data、runtime_session_event），示例输出正常。

- [ ] **Step 4: Commit**

```bash
git add IObject_规则书与设计评估.md IObject_后续关键能力清单.md
git commit -m "docs: document runtime domain and bridge model"
```
