# RuntimeBridge MessagePack 协议适配器实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 依据 `docs/superpowers/specs/2026-08-18-runtime-bridge-messagepack-protocol-design.md` 实现 MessagePack 协议适配器 `RuntimeBridgePeer`，把传输消息映射到已实现的 `RuntimeSession`，不含真实传输层。

**Architecture:** 新增公共头 `include/iobject/RuntimeBridgeProtocol.hpp` 与实现 `src/RuntimeBridgeProtocol.cpp`；MessagePack 编解码使用 vendored 的 msgpack11（`ar90n/msgpack11`，两文件，编译进 IObject 静态库，include 路径 PRIVATE）。`RuntimeSession` 仅增补 `RootObject()` 与 `HasObject()` 两个小方法。测试用进程内回环：构造请求 → `dump()` → `ReceiveMessage` → 解析出站帧断言。

**Tech Stack:** C++20、CMake、CTest、msgpack11（vendored 到 `third_party/msgpack11/`）。

**关键事实（已核实，执行者不要重新调研）：**

- msgpack11 源文件：`msgpack11.hpp` 与 `msgpack11.cpp`，许可证文件 `LICENSE.txt`，仓库 `https://github.com/ar90n/msgpack11`（master 分支）。本机访问 GitHub 需要代理：`curl -x http://127.0.0.1:7892`。
- `msgpack11::MsgPack`：`object = std::map<MsgPack, MsgPack>`，`binary = std::vector<uint8_t>`；构造支持 `const char*`/`std::string`/`uint64_t`/`bool`/`binary`/`object`；访问器 `is_object()`/`is_string()`/`is_binary()`/`string_value()`/`bool_value()`/`object_items()`/`binary_items()`；`operator[](const std::string&)` 对缺失键返回静态 NUL 值。
- 整数按宽度分类型（UINT8…UINT64、INT8…INT64），但 `int64_value()` 对所有整数类型都经 `static_cast` 可用（已读源码确认）；`type() & MsgPack::INT` 可判断任意整数类型。
- 序列化 `dump() → std::string`；反序列化 `MsgPack::parse(const std::string&, std::string& err)`，`err` 非空即失败。

**构建与测试命令（Windows / MSVC，复用现有 build/）：**

```bash
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

---

### Task 1: vendored msgpack11 接入与冒烟测试

**Files:**
- Create: `third_party/msgpack11/msgpack11.hpp`（下载）
- Create: `third_party/msgpack11/msgpack11.cpp`（下载）
- Create: `third_party/msgpack11/LICENSE.txt`（下载）
- Modify: `CMakeLists.txt`（库源文件与 PRIVATE include）
- Create: `tests/MsgPack11SmokeTest.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 下载 msgpack11 源码（需要代理）**

```bash
mkdir -p third_party/msgpack11
for f in msgpack11.hpp msgpack11.cpp LICENSE.txt; do
  curl -sL -x http://127.0.0.1:7892 -o "third_party/msgpack11/$f" \
    "https://raw.githubusercontent.com/ar90n/msgpack11/master/$f"
done
wc -c third_party/msgpack11/*
```

预期：三个文件都非空（hpp 约 9KB、cpp 约 40KB、LICENSE.txt 为 MIT 许可证文本）。若代理失效，先确认本机代理端口再重试，不得改用其他库。

- [ ] **Step 2: 修改 CMakeLists.txt**

库源文件列表改为：

```cmake
add_library(IObject STATIC
    src/RuntimeObject.cpp
    src/RuntimeDomain.cpp
    src/RuntimeBridge.cpp
    third_party/msgpack11/msgpack11.cpp
)
```

`target_include_directories(IObject ...)` 的 `PRIVATE` 段追加一行：

```cmake
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/msgpack11
```

- [ ] **Step 3: 编写冒烟测试**

`tests/MsgPack11SmokeTest.cpp`：

```cpp
#include "TestCheck.hpp"

#include <msgpack11.hpp>

#include <cstdint>
#include <string>

int main() {
    using msgpack11::MsgPack;

    const MsgPack message = MsgPack(MsgPack::object{
        {"op", MsgPack("ReadData")},
        {"id", MsgPack(std::uint64_t(42))},
        {"data", MsgPack(MsgPack::binary{1, 2, 3, 250})}});
    const std::string frame = message.dump();

    std::string err;
    const MsgPack parsed = MsgPack::parse(frame, err);
    TEST_CHECK(err.empty());
    TEST_CHECK(parsed.is_object());
    TEST_CHECK(parsed["op"].string_value() == "ReadData");
    // 小整数会解析为窄无符号类型；int64_value 对任意整数类型可用。
    TEST_CHECK(parsed["id"].int64_value() == 42);
    TEST_CHECK(parsed["data"].is_binary());
    TEST_CHECK(parsed["data"].binary_items().size() == 4);
    TEST_CHECK(parsed["data"].binary_items()[3] == 250);
    return 0;
}
```

`tests/CMakeLists.txt` 追加：

```cmake
add_executable(test_msgpack11_smoke MsgPack11SmokeTest.cpp)
target_link_libraries(test_msgpack11_smoke PRIVATE IObject::IObject)
target_include_directories(test_msgpack11_smoke PRIVATE ${CMAKE_SOURCE_DIR}/third_party/msgpack11)
add_test(NAME msgpack11_smoke COMMAND test_msgpack11_smoke)
```

- [ ] **Step 4: 构建并运行测试**

```bash
cmake -S . -B build && cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

预期：6/6 通过（含 msgpack11_smoke）。若 msgpack11.cpp 在 C++20/MSVC 下有编译错误，先记录错误再决定是否加编译选项，不要修改 msgpack11 源码。

- [ ] **Step 5: Commit**

```bash
git add third_party/msgpack11 CMakeLists.txt tests/MsgPack11SmokeTest.cpp tests/CMakeLists.txt
git commit -m "build: vendor msgpack11 for bridge protocol codec"
```

---

### Task 2: RuntimeSession 增补 RootObject 与 HasObject

**Files:**
- Modify: `include/iobject/RuntimeBridge.hpp`（RuntimeSession 增两个方法）
- Modify: `src/RuntimeBridge.cpp`（构造注册根锚点 + 两个方法实现）
- Create: `tests/RuntimeSessionRootTest.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 编写失败测试**

`tests/RuntimeSessionRootTest.cpp`：

```cpp
#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <memory>

int main() {
    iobject::RuntimeDomain domain;
    iobject::IRuntimeObject* player = iobject::Runtime::make();
    TEST_CHECK(domain.RootAnchor()->Connect("Player", player));

    std::unique_ptr<iobject::RuntimeSession> session = domain.BridgeRoot().OpenSession();
    const iobject::RemoteObjectHandle root = session->RootObject();
    TEST_CHECK(root != 0);
    TEST_CHECK(session->HasObject(root));
    TEST_CHECK(!session->HasObject(999999));

    // 根 addr 可以直接发现子对象。
    const iobject::RemoteObjectHandle playerHandle = session->ResolveChild(root, "Player");
    TEST_CHECK(playerHandle != 0);
    TEST_CHECK(session->HasObject(playerHandle));

    session->Close();
    TEST_CHECK(session->RootObject() == 0);
    TEST_CHECK(!session->HasObject(root));

    delete player;
    return 0;
}
```

`tests/CMakeLists.txt` 追加：

```cmake
add_executable(test_runtime_session_root RuntimeSessionRootTest.cpp)
target_link_libraries(test_runtime_session_root PRIVATE IObject::IObject)
add_test(NAME runtime_session_root COMMAND test_runtime_session_root)
```

- [ ] **Step 2: 构建验证编译失败**（`RootObject`/`HasObject` 未定义）

- [ ] **Step 3: 实现**

`include/iobject/RuntimeBridge.hpp`：在 `RuntimeSession` 的 `IsOpen()` 声明之后追加：

```cpp
    /// 根锚点在会话内的句柄；会话打开期间有效，关闭后返回 0。
    RemoteObjectHandle RootObject() const noexcept;
    /// 句柄有效性查询：会话打开且句柄已登记时返回 true。
    bool HasObject(RemoteObjectHandle handle) const noexcept;
```

`src/RuntimeBridge.cpp` 三处改动：

1. `RuntimeSession::Impl` 增加成员（放在 `rootAnchor` 声明之后）：

```cpp
    RemoteObjectHandle rootHandle = 0;  // 构造时登记的根锚点句柄。
```

2. 构造函数改为登记根锚点：

```cpp
RuntimeSession::RuntimeSession(IRuntimeObject* rootAnchor, IRuntimeObject* relay)
    : impl_(std::make_unique<Impl>()) {
    impl_->rootAnchor = rootAnchor;
    impl_->relay = relay;
    impl_->rootHandle = impl_->registerObject(rootAnchor);
}
```

3. 文件末尾（`IsOpen` 定义之后）追加：

```cpp
RemoteObjectHandle RuntimeSession::RootObject() const noexcept {
    return impl_->open ? impl_->rootHandle : 0;
}

bool RuntimeSession::HasObject(RemoteObjectHandle handle) const noexcept {
    return impl_->open
        && impl_->objectsByHandle.find(handle) != impl_->objectsByHandle.end();
}
```

- [ ] **Step 4: 构建并运行测试**

预期：7/7 通过（含 runtime_session_root）。

- [ ] **Step 5: Commit**

```bash
git add include/iobject/RuntimeBridge.hpp src/RuntimeBridge.cpp tests/RuntimeSessionRootTest.cpp tests/CMakeLists.txt
git commit -m "feat: expose session root object handle and validity query"
```

---

### Task 3: 协议适配器骨架、Connect 握手与信封校验

**Files:**
- Create: `include/iobject/RuntimeBridgeProtocol.hpp`
- Create: `src/RuntimeBridgeProtocol.cpp`
- Modify: `CMakeLists.txt`（库源文件 + 安装头）
- Create: `tests/RuntimeBridgeProtocolConnectTest.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 编写失败测试**

`tests/RuntimeBridgeProtocolConnectTest.cpp`：

```cpp
#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeBridgeProtocol.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <msgpack11.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using msgpack11::MsgPack;

// 进程内回环：出站帧收集到 outbox，供解析断言。
struct Loopback {
    iobject::RuntimeDomain domain;
    std::vector<std::string> outbox;
    std::unique_ptr<iobject::RuntimeBridgePeer> peer;

    Loopback()
        : peer(std::make_unique<iobject::RuntimeBridgePeer>(
              domain.BridgeRoot(), "MainScene",
              [this](iobject::ByteView frame) {
                  outbox.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
              })) {}

    void send(const MsgPack& message) {
        const std::string frame = message.dump();
        peer->ReceiveMessage(
            iobject::ByteView(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()));
    }

    void sendRaw(const void* data, std::size_t size) {
        peer->ReceiveMessage(iobject::ByteView(static_cast<const std::uint8_t*>(data), size));
    }

    MsgPack lastFrame() {
        TEST_CHECK(!outbox.empty());
        std::string err;
        const MsgPack frame = MsgPack::parse(outbox.back(), err);
        TEST_CHECK(err.empty());
        return frame;
    }

    MsgPack roundtrip(const MsgPack& request) {
        send(request);
        return lastFrame();
    }

    MsgPack request(std::uint64_t id, const char* op, MsgPack::object fields = {}) {
        MsgPack::object object{{"id", MsgPack(id)}, {"op", MsgPack(op)}};
        for (auto& [key, value] : fields) {
            object.emplace(key, std::move(value));
        }
        return MsgPack(std::move(object));
    }
};

std::string errorCode(const MsgPack& response) {
    return response["error"]["code"].string_value();
}

} // namespace

int main() {
    // 1. 握手成功：返回非零 root addr。
    {
        Loopback loop;
        const MsgPack response =
            loop.roundtrip(loop.request(1, "Connect", {{"domain", MsgPack("MainScene")}}));
        TEST_CHECK(response["ok"].bool_value());
        TEST_CHECK(response["root"].int64_value() > 0);
        TEST_CHECK(loop.peer->IsOpen());
    }

    // 2. 域名不匹配：DomainNotFound，随后连接关闭，后续消息被忽略。
    {
        Loopback loop;
        const MsgPack response =
            loop.roundtrip(loop.request(1, "Connect", {{"domain", MsgPack("Wrong")}}));
        TEST_CHECK(!response["ok"].bool_value());
        TEST_CHECK(errorCode(response) == "DomainNotFound");
        TEST_CHECK(!loop.peer->IsOpen());
        const std::size_t frames = loop.outbox.size();
        loop.send(loop.request(2, "Connect", {{"domain", MsgPack("MainScene")}}));
        TEST_CHECK(loop.outbox.size() == frames);
    }

    // 3. 握手前发送其他操作：SessionNotEstablished。
    {
        Loopback loop;
        const MsgPack response = loop.roundtrip(loop.request(1, "GetChildItem"));
        TEST_CHECK(!response["ok"].bool_value());
        TEST_CHECK(errorCode(response) == "SessionNotEstablished");
    }

    // 4. 非法字节流：MalformedMessage，id 为 0。
    {
        Loopback loop;
        const std::uint8_t garbage[2] = {0xC1, 0xFF};  // 0xC1 是 MessagePack 保留字节
        loop.sendRaw(garbage, sizeof(garbage));
        const MsgPack response = loop.lastFrame();
        TEST_CHECK(!response["ok"].bool_value());
        TEST_CHECK(response["id"].int64_value() == 0);
        TEST_CHECK(errorCode(response) == "MalformedMessage");
    }

    // 5. 非 map 消息：MalformedMessage，id 为 0。
    {
        Loopback loop;
        loop.send(MsgPack(std::uint64_t(42)));
        TEST_CHECK(errorCode(loop.lastFrame()) == "MalformedMessage");
    }

    // 6. 缺 id：MalformedMessage 且 id 为 0；缺 op：MalformedMessage 且回显 id。
    {
        Loopback loop;
        loop.send(MsgPack(MsgPack::object{{"op", MsgPack("Connect")}}));
        const MsgPack noId = loop.lastFrame();
        TEST_CHECK(errorCode(noId) == "MalformedMessage");
        TEST_CHECK(noId["id"].int64_value() == 0);

        loop.send(MsgPack(MsgPack::object{{"id", MsgPack(std::uint64_t(7))}}));
        const MsgPack noOp = loop.lastFrame();
        TEST_CHECK(errorCode(noOp) == "MalformedMessage");
        TEST_CHECK(noOp["id"].int64_value() == 7);
    }

    // 7. 重复 Connect：OperationFailed。
    {
        Loopback loop;
        loop.roundtrip(loop.request(1, "Connect", {{"domain", MsgPack("MainScene")}}));
        const MsgPack again =
            loop.roundtrip(loop.request(2, "Connect", {{"domain", MsgPack("MainScene")}}));
        TEST_CHECK(errorCode(again) == "OperationFailed");
    }

    // 8. 未知操作：UnknownOp。
    {
        Loopback loop;
        loop.roundtrip(loop.request(1, "Connect", {{"domain", MsgPack("MainScene")}}));
        const MsgPack response = loop.roundtrip(loop.request(2, "Fly"));
        TEST_CHECK(errorCode(response) == "UnknownOp");
    }

    // 9. 超长消息（> 1 MiB）：MalformedMessage 并关闭连接。
    {
        Loopback loop;
        const MsgPack huge =
            loop.request(1, "Connect", {{"domain", MsgPack(std::string(2 * 1024 * 1024, 'x'))}});
        loop.send(huge);
        const MsgPack response = loop.lastFrame();
        TEST_CHECK(errorCode(response) == "MalformedMessage");
        TEST_CHECK(!loop.peer->IsOpen());
    }

    // 10. Close：先回 ok 再关闭；之后消息被忽略。
    {
        Loopback loop;
        loop.roundtrip(loop.request(1, "Connect", {{"domain", MsgPack("MainScene")}}));
        const MsgPack response = loop.roundtrip(loop.request(2, "Close"));
        TEST_CHECK(response["ok"].bool_value());
        TEST_CHECK(!loop.peer->IsOpen());
        const std::size_t frames = loop.outbox.size();
        loop.send(loop.request(3, "Close"));
        TEST_CHECK(loop.outbox.size() == frames);
    }
    return 0;
}
```

`tests/CMakeLists.txt` 追加：

```cmake
add_executable(test_runtime_bridge_protocol_connect RuntimeBridgeProtocolConnectTest.cpp)
target_link_libraries(test_runtime_bridge_protocol_connect PRIVATE IObject::IObject)
target_include_directories(test_runtime_bridge_protocol_connect PRIVATE ${CMAKE_SOURCE_DIR}/third_party/msgpack11)
add_test(NAME runtime_bridge_protocol_connect COMMAND test_runtime_bridge_protocol_connect)
```

- [ ] **Step 2: 构建验证编译失败**（`RuntimeBridgeProtocol.hpp` 不存在）

- [ ] **Step 3: 实现公共头**

`include/iobject/RuntimeBridgeProtocol.hpp`：

```cpp
#pragma once

#include "IRuntimeObject.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace iobject {

class RuntimeBridgeRoot;

/// MessagePack 协议适配器：把一个传输连接映射到一个 RuntimeSession。
/// 传输层每收到一条完整消息就调用 ReceiveMessage；适配器经 SendCallback 发回响应帧与事件帧。
/// 帧字节仅在 SendCallback 调用期间有效，传输层需要保留时必须自行复制。
/// 非线程安全，与框架其余部分一样假定单线程事件循环。
class RuntimeBridgePeer final {
public:
    /// 传输层发送一帧（完整 MessagePack 文档）给客户端的回调。
    using SendCallback = std::function<void(ByteView frame)>;

    /// domain 是本连接对应的域名；Connect 请求的 domain 不匹配时握手失败并关闭连接。
    RuntimeBridgePeer(RuntimeBridgeRoot& bridgeRoot, std::string domain, SendCallback send);
    ~RuntimeBridgePeer();

    RuntimeBridgePeer(const RuntimeBridgePeer&) = delete;
    RuntimeBridgePeer& operator=(const RuntimeBridgePeer&) = delete;

    /// 传输层收到一条完整消息时调用；畸形消息回 MalformedMessage，超长消息回错并关闭连接。
    void ReceiveMessage(ByteView message);
    /// 关闭连接：关闭底层会话，之后不再发送任何帧；幂等。传输断开时必须调用。
    void Close() noexcept;
    bool IsOpen() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iobject
```

修改 `CMakeLists.txt`：库源文件追加 `src/RuntimeBridgeProtocol.cpp`；安装头列表追加 `include/iobject/RuntimeBridgeProtocol.hpp`。

- [ ] **Step 4: 实现适配器骨架**

`src/RuntimeBridgeProtocol.cpp`（本任务的完整文件；后续任务在此追加 handler）：

```cpp
#include <iobject/RuntimeBridgeProtocol.hpp>

#include <iobject/RuntimeBridge.hpp>

#include <msgpack11.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <utility>

namespace iobject {
namespace {

using msgpack11::MsgPack;

constexpr std::size_t MaxMessageSize = 1024 * 1024;  // 协议建议的单条消息上限 1 MiB。

// msgpack11 按宽度区分整数类型；int64_value 对所有整数类型可用（已核实源码 static_cast）。
// 协议承诺数值不超过 2^53，int64 范围足够；负数拒绝。
bool asUint64(const MsgPack& value, std::uint64_t& out) {
    if (!(value.type() & MsgPack::INT)) {
        return false;
    }
    const std::int64_t signedValue = value.int64_value();
    if (signedValue < 0) {
        return false;
    }
    out = static_cast<std::uint64_t>(signedValue);
    return true;
}

bool asString(const MsgPack& value, std::string& out) {
    if (!value.is_string()) {
        return false;
    }
    out = value.string_value();
    return true;
}

MsgPack okResponse(std::uint64_t id, MsgPack::object fields = {}) {
    MsgPack::object object{{"id", MsgPack(id)}, {"ok", MsgPack(true)}};
    for (auto& [key, value] : fields) {
        object.emplace(key, std::move(value));
    }
    return MsgPack(std::move(object));
}

MsgPack errorResponse(std::uint64_t id, const char* code, const std::string& message) {
    return MsgPack(MsgPack::object{
        {"id", MsgPack(id)},
        {"ok", MsgPack(false)},
        {"error", MsgPack(MsgPack::object{
            {"code", MsgPack(code)},
            {"message", MsgPack(message)}})}});
}

} // namespace

struct RuntimeBridgePeer::Impl {
    RuntimeBridgeRoot* bridgeRoot = nullptr;  // 非拥有。
    std::string domain;
    RuntimeBridgePeer::SendCallback send;
    std::unique_ptr<RuntimeSession> session;
    std::set<std::uint64_t> subscriptions;
    bool open = true;

    void sendMessage(const MsgPack& message) {
        if (!open || !send) {
            return;
        }
        const std::string frame = message.dump();
        send(ByteView(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()));
    }

    void close() noexcept {
        if (!open) {
            return;
        }
        open = false;
        subscriptions.clear();
        if (session != nullptr) {
            session->Close();
            session.reset();
        }
    }

    void receive(ByteView message) {
        if (!open) {
            return;
        }
        if (message.size() > MaxMessageSize) {
            sendMessage(errorResponse(0, "MalformedMessage", "消息超过 1 MiB 上限"));
            close();
            return;
        }

        std::string parseError;
        const MsgPack request = MsgPack::parse(
            std::string(reinterpret_cast<const char*>(message.data()), message.size()),
            parseError);
        if (!parseError.empty() || !request.is_object()) {
            sendMessage(errorResponse(0, "MalformedMessage", "消息不是合法的 MessagePack map"));
            return;
        }

        std::uint64_t id = 0;
        if (!asUint64(request["id"], id) || id == 0) {
            sendMessage(errorResponse(0, "MalformedMessage", "缺少合法的 id 字段"));
            return;
        }
        std::string op;
        if (!asString(request["op"], op) || op.empty()) {
            sendMessage(errorResponse(id, "MalformedMessage", "缺少合法的 op 字段"));
            return;
        }

        if (session == nullptr && op != "Connect") {
            sendMessage(errorResponse(id, "SessionNotEstablished", "握手成功前只接受 Connect"));
            return;
        }

        if (op == "Connect") {
            handleConnect(id, request);
        } else if (op == "Close") {
            handleClose(id);
        } else {
            sendMessage(errorResponse(id, "UnknownOp", "未知操作: " + op));
        }
    }

    void handleConnect(std::uint64_t id, const MsgPack& request) {
        if (session != nullptr) {
            sendMessage(errorResponse(id, "OperationFailed", "会话已建立，忽略重复 Connect"));
            return;
        }
        std::string domain;
        if (!asString(request["domain"], domain) || domain.empty()) {
            sendMessage(errorResponse(id, "MalformedMessage", "缺少合法的 domain 字段"));
            return;
        }
        if (domain != this->domain) {
            sendMessage(errorResponse(id, "DomainNotFound", "域不存在: " + domain));
            close();
            return;
        }
        session = bridgeRoot->OpenSession();
        if (session == nullptr || session->RootObject() == 0) {
            session.reset();
            sendMessage(errorResponse(id, "OperationFailed", "桥接不可用"));
            return;
        }
        sendMessage(okResponse(id, {{"root", MsgPack(session->RootObject())}}));
    }

    void handleClose(std::uint64_t id) {
        sendMessage(okResponse(id));
        close();
    }
};

RuntimeBridgePeer::RuntimeBridgePeer(RuntimeBridgeRoot& bridgeRoot, std::string domain,
                                     SendCallback send)
    : impl_(std::make_unique<Impl>()) {
    impl_->bridgeRoot = &bridgeRoot;
    impl_->domain = std::move(domain);
    impl_->send = std::move(send);
}

RuntimeBridgePeer::~RuntimeBridgePeer() {
    impl_->close();
}

void RuntimeBridgePeer::ReceiveMessage(ByteView message) {
    impl_->receive(message);
}

void RuntimeBridgePeer::Close() noexcept {
    impl_->close();
}

bool RuntimeBridgePeer::IsOpen() const noexcept {
    return impl_->open;
}

} // namespace iobject
```

- [ ] **Step 5: 构建并运行测试**

预期：8/8 通过（含 runtime_bridge_protocol_connect）。

- [ ] **Step 6: Commit**

```bash
git add include/iobject/RuntimeBridgeProtocol.hpp src/RuntimeBridgeProtocol.cpp CMakeLists.txt tests/RuntimeBridgeProtocolConnectTest.cpp tests/CMakeLists.txt
git commit -m "feat: add bridge protocol peer with connect handshake"
```

---

### Task 4: GetChildItem 操作

**Files:**
- Modify: `src/RuntimeBridgeProtocol.cpp`（dispatch 分支 + handler）
- Create: `tests/RuntimeBridgeProtocolObjectTest.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 编写失败测试**

`tests/RuntimeBridgeProtocolObjectTest.cpp`：

```cpp
#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeBridgeProtocol.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <msgpack11.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using msgpack11::MsgPack;

struct Loopback {
    iobject::RuntimeDomain domain;
    std::vector<std::string> outbox;
    std::unique_ptr<iobject::RuntimeBridgePeer> peer;
    std::uint64_t root = 0;
    std::uint64_t nextId = 1;

    Loopback()
        : peer(std::make_unique<iobject::RuntimeBridgePeer>(
              domain.BridgeRoot(), "MainScene",
              [this](iobject::ByteView frame) {
                  outbox.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
              })) {
        const MsgPack response = roundtrip(request(
            MsgPack::object{{"op", MsgPack("Connect")}, {"domain", MsgPack("MainScene")}}));
        TEST_CHECK(response["ok"].bool_value());
        root = static_cast<std::uint64_t>(response["root"].int64_value());
        TEST_CHECK(root != 0);
    }

    void send(const MsgPack& message) {
        const std::string frame = message.dump();
        peer->ReceiveMessage(
            iobject::ByteView(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()));
    }

    MsgPack lastFrame() {
        TEST_CHECK(!outbox.empty());
        std::string err;
        const MsgPack frame = MsgPack::parse(outbox.back(), err);
        TEST_CHECK(err.empty());
        return frame;
    }

    MsgPack request(MsgPack::object object) {
        object.emplace(MsgPack("id"), MsgPack(nextId++));
        return MsgPack(std::move(object));
    }

    MsgPack roundtrip(const MsgPack& message) {
        send(message);
        return lastFrame();
    }

    MsgPack getChildItem(std::uint64_t addr, const std::string& childId) {
        return roundtrip(request({{"op", MsgPack("GetChildItem")},
                                  {"addr", MsgPack(addr)},
                                  {"childId", MsgPack(childId)}}));
    }
};

std::string errorCode(const MsgPack& response) {
    return response["error"]["code"].string_value();
}

} // namespace

int main() {
    iobject::IRuntimeObject* keepAlive = nullptr;
    {
        Loopback loop;
        iobject::IRuntimeObject* player = iobject::Runtime::make();
        iobject::IRuntimeObject* decoder = iobject::Runtime::make();
        keepAlive = decoder;
        TEST_CHECK(loop.domain.RootAnchor()->Connect("Player", player));
        TEST_CHECK(player->Connect("Decoder", decoder));

        // 逐级解析：root -> Player -> Decoder，响应回显 childId。
        const MsgPack playerResponse = loop.getChildItem(loop.root, "Player");
        TEST_CHECK(playerResponse["ok"].bool_value());
        TEST_CHECK(playerResponse["childId"].string_value() == "Player");
        const std::uint64_t playerAddr =
            static_cast<std::uint64_t>(playerResponse["addr"].int64_value());
        TEST_CHECK(playerAddr != 0);

        const MsgPack decoderResponse = loop.getChildItem(playerAddr, "Decoder");
        TEST_CHECK(decoderResponse["ok"].bool_value());
        TEST_CHECK(decoderResponse["childId"].string_value() == "Decoder");
        TEST_CHECK(decoderResponse["addr"].int64_value() > 0);

        // 子对象不存在：ObjectNotFound。
        TEST_CHECK(errorCode(loop.getChildItem(loop.root, "Missing")) == "ObjectNotFound");

        // addr 无效：AddrInvalid。
        TEST_CHECK(errorCode(loop.getChildItem(999999, "Player")) == "AddrInvalid");

        // childId 含 '.' 或为空：MalformedMessage（协议规定 childId 是单层名称）。
        TEST_CHECK(errorCode(loop.getChildItem(loop.root, "Player.Decoder")) == "MalformedMessage");
        TEST_CHECK(errorCode(loop.getChildItem(loop.root, "")) == "MalformedMessage");

        delete player;  // decoder 随 player 的拓扑解除而独立存活，稍后删除。
    }
    delete keepAlive;
    return 0;
}
```

注意：测试里 `delete player` 时 decoder 仍是其子节点，内核会自动解除拓扑；`keepAlive` 保存 decoder 指针用于最后删除。这依赖既有内核语义（delete 父节点不会 delete 子节点）。

`tests/CMakeLists.txt` 追加：

```cmake
add_executable(test_runtime_bridge_protocol_object RuntimeBridgeProtocolObjectTest.cpp)
target_link_libraries(test_runtime_bridge_protocol_object PRIVATE IObject::IObject)
target_include_directories(test_runtime_bridge_protocol_object PRIVATE ${CMAKE_SOURCE_DIR}/third_party/msgpack11)
add_test(NAME runtime_bridge_protocol_object COMMAND test_runtime_bridge_protocol_object)
```

- [ ] **Step 2: 构建验证失败**（GetChildItem 返回 UnknownOp，测试断言失败）

- [ ] **Step 3: 实现**

`src/RuntimeBridgeProtocol.cpp` 两处改动：

1. `receive` 的 dispatch 中，`} else if (op == "Close") {` 之前插入：

```cpp
        } else if (op == "GetChildItem") {
            handleGetChildItem(id, request);
```

2. `Impl` 中 `handleConnect` 之后追加：

```cpp
    void handleGetChildItem(std::uint64_t id, const MsgPack& request) {
        std::uint64_t addr = 0;
        std::string childId;
        if (!asUint64(request["addr"], addr) || addr == 0
            || !asString(request["childId"], childId)) {
            sendMessage(errorResponse(id, "MalformedMessage", "缺少合法的 addr 或 childId 字段"));
            return;
        }
        if (childId.empty() || childId.find('.') != std::string::npos) {
            sendMessage(errorResponse(id, "MalformedMessage", "childId 必须是非空单层名称"));
            return;
        }
        if (!session->HasObject(addr)) {
            sendMessage(errorResponse(id, "AddrInvalid", "addr 无效或已失效"));
            return;
        }
        const RemoteObjectHandle child = session->ResolveChild(addr, childId);
        if (child == 0) {
            sendMessage(errorResponse(id, "ObjectNotFound", "子对象不存在: " + childId));
            return;
        }
        sendMessage(okResponse(id, {{"childId", MsgPack(childId)}, {"addr", MsgPack(child)}}));
    }
```

- [ ] **Step 4: 构建并运行测试**

预期：9/9 通过（含 runtime_bridge_protocol_object）。

- [ ] **Step 5: Commit**

```bash
git add src/RuntimeBridgeProtocol.cpp tests/RuntimeBridgeProtocolObjectTest.cpp tests/CMakeLists.txt
git commit -m "feat: add GetChildItem operation to bridge protocol"
```

---

### Task 5: ReadData / WriteData 操作

**Files:**
- Modify: `src/RuntimeBridgeProtocol.cpp`（dispatch 分支 + 两个 handler）
- Create: `tests/RuntimeBridgeProtocolDataTest.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 编写失败测试**

`tests/RuntimeBridgeProtocolDataTest.cpp`：

```cpp
#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeBridgeProtocol.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <msgpack11.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using msgpack11::MsgPack;

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

struct Loopback {
    iobject::RuntimeDomain domain;
    std::vector<std::string> outbox;
    std::unique_ptr<iobject::RuntimeBridgePeer> peer;
    std::uint64_t root = 0;
    std::uint64_t nextId = 1;

    Loopback()
        : peer(std::make_unique<iobject::RuntimeBridgePeer>(
              domain.BridgeRoot(), "MainScene",
              [this](iobject::ByteView frame) {
                  outbox.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
              })) {
        const MsgPack response = roundtrip(request(
            {{"op", MsgPack("Connect")}, {"domain", MsgPack("MainScene")}}));
        TEST_CHECK(response["ok"].bool_value());
        root = static_cast<std::uint64_t>(response["root"].int64_value());
        TEST_CHECK(root != 0);
    }

    void send(const MsgPack& message) {
        const std::string frame = message.dump();
        peer->ReceiveMessage(
            iobject::ByteView(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()));
    }

    MsgPack lastFrame() {
        TEST_CHECK(!outbox.empty());
        std::string err;
        const MsgPack frame = MsgPack::parse(outbox.back(), err);
        TEST_CHECK(err.empty());
        return frame;
    }

    MsgPack request(MsgPack::object object) {
        object.emplace(MsgPack("id"), MsgPack(nextId++));
        return MsgPack(std::move(object));
    }

    MsgPack roundtrip(const MsgPack& message) {
        send(message);
        return lastFrame();
    }

    MsgPack call(const char* op, MsgPack::object fields) {
        MsgPack::object object{{"op", MsgPack(op)}};
        for (auto& [key, value] : fields) {
            object.emplace(key, std::move(value));
        }
        return roundtrip(request(std::move(object)));
    }
};

std::string errorCode(const MsgPack& response) {
    return response["error"]["code"].string_value();
}

} // namespace

int main() {
    Loopback loop;
    iobject::IRuntimeObject* device = iobject::Runtime::make<Device>();
    TEST_CHECK(loop.domain.RootAnchor()->Connect("Device", device));

    const MsgPack found = loop.call("GetChildItem",
        {{"addr", MsgPack(loop.root)}, {"childId", MsgPack("Device")}});
    TEST_CHECK(found["ok"].bool_value());
    const std::uint64_t addr = static_cast<std::uint64_t>(found["addr"].int64_value());
    TEST_CHECK(addr != 0);

    // 写入 300（小端四字节），再读回。
    const MsgPack written = loop.call("WriteData",
        {{"addr", MsgPack(addr)},
         {"channel", MsgPack("State")},
         {"data", MsgPack(MsgPack::binary{0x2C, 0x01, 0x00, 0x00})}});
    TEST_CHECK(written["ok"].bool_value());

    const MsgPack read = loop.call("ReadData",
        {{"addr", MsgPack(addr)}, {"channel", MsgPack("State")}});
    TEST_CHECK(read["ok"].bool_value());
    TEST_CHECK(read["data"].is_binary());
    const MsgPack::binary& bytes = read["data"].binary_items();
    TEST_CHECK(bytes.size() == 4);
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
    TEST_CHECK(value == 300);

    // 未知通道：OperationFailed。
    TEST_CHECK(errorCode(loop.call("ReadData",
        {{"addr", MsgPack(addr)}, {"channel", MsgPack("Missing")}})) == "OperationFailed");

    // addr 无效：AddrInvalid。
    TEST_CHECK(errorCode(loop.call("ReadData",
        {{"addr", MsgPack(std::uint64_t(999999))}, {"channel", MsgPack("State")}}))
        == "AddrInvalid");

    // data 字段不是二进制：MalformedMessage。
    TEST_CHECK(errorCode(loop.call("WriteData",
        {{"addr", MsgPack(addr)},
         {"channel", MsgPack("State")},
         {"data", MsgPack("not-binary")}})) == "MalformedMessage");

    delete device;
    return 0;
}
```

`tests/CMakeLists.txt` 追加：

```cmake
add_executable(test_runtime_bridge_protocol_data RuntimeBridgeProtocolDataTest.cpp)
target_link_libraries(test_runtime_bridge_protocol_data PRIVATE IObject::IObject)
target_include_directories(test_runtime_bridge_protocol_data PRIVATE ${CMAKE_SOURCE_DIR}/third_party/msgpack11)
add_test(NAME runtime_bridge_protocol_data COMMAND test_runtime_bridge_protocol_data)
```

- [ ] **Step 2: 构建验证失败**（UnknownOp）

- [ ] **Step 3: 实现**

`src/RuntimeBridgeProtocol.cpp` 两处改动：

1. dispatch 中 `} else if (op == "Close") {` 之前插入：

```cpp
        } else if (op == "ReadData") {
            handleReadData(id, request);
        } else if (op == "WriteData") {
            handleWriteData(id, request);
```

2. `Impl` 中追加：

```cpp
    void handleReadData(std::uint64_t id, const MsgPack& request) {
        std::uint64_t addr = 0;
        std::string channel;
        if (!asUint64(request["addr"], addr) || addr == 0
            || !asString(request["channel"], channel)) {
            sendMessage(errorResponse(id, "MalformedMessage", "缺少合法的 addr 或 channel 字段"));
            return;
        }
        if (!session->HasObject(addr)) {
            sendMessage(errorResponse(id, "AddrInvalid", "addr 无效或已失效"));
            return;
        }
        MsgPack::binary bytes;
        const bool accepted = session->ReadData(addr, channel, [&bytes](ByteView view) {
            bytes.assign(view.begin(), view.end());
        });
        if (!accepted) {
            sendMessage(errorResponse(id, "OperationFailed", "对象拒绝读取通道: " + channel));
            return;
        }
        sendMessage(okResponse(id, {{"data", MsgPack(std::move(bytes))}}));
    }

    void handleWriteData(std::uint64_t id, const MsgPack& request) {
        std::uint64_t addr = 0;
        std::string channel;
        if (!asUint64(request["addr"], addr) || addr == 0
            || !asString(request["channel"], channel)) {
            sendMessage(errorResponse(id, "MalformedMessage", "缺少合法的 addr 或 channel 字段"));
            return;
        }
        const MsgPack& data = request["data"];
        if (!data.is_binary()) {
            sendMessage(errorResponse(id, "MalformedMessage", "data 字段必须是二进制"));
            return;
        }
        if (!session->HasObject(addr)) {
            sendMessage(errorResponse(id, "AddrInvalid", "addr 无效或已失效"));
            return;
        }
        const MsgPack::binary& bytes = data.binary_items();
        const bool accepted = session->WriteData(
            addr, channel, ByteView(bytes.data(), bytes.size()));
        if (!accepted) {
            sendMessage(errorResponse(id, "OperationFailed", "对象拒绝写入通道: " + channel));
            return;
        }
        sendMessage(okResponse(id));
    }
```

- [ ] **Step 4: 构建并运行测试**

预期：10/10 通过（含 runtime_bridge_protocol_data）。

- [ ] **Step 5: Commit**

```bash
git add src/RuntimeBridgeProtocol.cpp tests/RuntimeBridgeProtocolDataTest.cpp tests/CMakeLists.txt
git commit -m "feat: add data channel operations to bridge protocol"
```

---

### Task 6: SubscribeEvent / CancelEvent / 事件下行帧 / Released

**Files:**
- Modify: `src/RuntimeBridgeProtocol.cpp`（dispatch 分支 + 两个 handler）
- Create: `tests/RuntimeBridgeProtocolEventTest.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 编写失败测试**

`tests/RuntimeBridgeProtocolEventTest.cpp`：

```cpp
#include "TestCheck.hpp"

#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeBridgeProtocol.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <msgpack11.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using msgpack11::MsgPack;

class Counter final {
public:
    void Increase() {
        ++value_;
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

struct Loopback {
    iobject::RuntimeDomain domain;
    std::vector<std::string> outbox;
    std::unique_ptr<iobject::RuntimeBridgePeer> peer;
    std::uint64_t root = 0;
    std::uint64_t nextId = 1;

    Loopback()
        : peer(std::make_unique<iobject::RuntimeBridgePeer>(
              domain.BridgeRoot(), "MainScene",
              [this](iobject::ByteView frame) {
                  outbox.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
              })) {
        const MsgPack response = roundtrip(request(
            {{"op", MsgPack("Connect")}, {"domain", MsgPack("MainScene")}}));
        TEST_CHECK(response["ok"].bool_value());
        root = static_cast<std::uint64_t>(response["root"].int64_value());
        TEST_CHECK(root != 0);
    }

    void send(const MsgPack& message) {
        const std::string frame = message.dump();
        peer->ReceiveMessage(
            iobject::ByteView(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()));
    }

    MsgPack lastFrame() {
        TEST_CHECK(!outbox.empty());
        std::string err;
        const MsgPack frame = MsgPack::parse(outbox.back(), err);
        TEST_CHECK(err.empty());
        return frame;
    }

    MsgPack request(MsgPack::object object) {
        object.emplace(MsgPack("id"), MsgPack(nextId++));
        return MsgPack(std::move(object));
    }

    MsgPack roundtrip(const MsgPack& message) {
        send(message);
        return lastFrame();
    }

    MsgPack call(const char* op, MsgPack::object fields) {
        MsgPack::object object{{"op", MsgPack(op)}};
        for (auto& [key, value] : fields) {
            object.emplace(key, std::move(value));
        }
        return roundtrip(request(std::move(object)));
    }
};

std::string errorCode(const MsgPack& response) {
    return response["error"]["code"].string_value();
}

} // namespace

int main() {
    Loopback loop;
    auto* counter = new Counter();
    iobject::IRuntimeObject* node = iobject::Runtime::fromPtr(counter, true);
    counter->Attach(node);
    TEST_CHECK(loop.domain.RootAnchor()->Connect("Counter", node));

    const MsgPack found = loop.call("GetChildItem",
        {{"addr", MsgPack(loop.root)}, {"childId", MsgPack("Counter")}});
    const std::uint64_t addr = static_cast<std::uint64_t>(found["addr"].int64_value());
    TEST_CHECK(addr != 0);

    // 1. 订阅 DataChannelChanged：返回非零 subscription。
    const MsgPack subscribed = loop.call("SubscribeEvent",
        {{"addr", MsgPack(addr)}, {"type", MsgPack("DataChannelChanged")}});
    TEST_CHECK(subscribed["ok"].bool_value());
    const std::uint64_t subscription =
        static_cast<std::uint64_t>(subscribed["subscription"].int64_value());
    TEST_CHECK(subscription != 0);

    // 2. C++ 侧发布：事件下行帧带 event/subscription/addr/channel。
    const std::size_t framesBefore = loop.outbox.size();
    counter->Increase();
    TEST_CHECK(loop.outbox.size() == framesBefore + 1);
    const MsgPack event = loop.lastFrame();
    TEST_CHECK(event["event"].string_value() == "DataChannelChanged");
    TEST_CHECK(static_cast<std::uint64_t>(event["subscription"].int64_value()) == subscription);
    TEST_CHECK(static_cast<std::uint64_t>(event["addr"].int64_value()) == addr);
    TEST_CHECK(event["channel"].string_value() == "Value");

    // 3. CancelEvent：ok；再次取消同一订阅：SubscriptionInvalid。
    TEST_CHECK(loop.call("CancelEvent",
        {{"subscription", MsgPack(subscription)}})["ok"].bool_value());
    const std::size_t framesAfterCancel = loop.outbox.size();
    counter->Increase();
    TEST_CHECK(loop.outbox.size() == framesAfterCancel);  // 不再收到事件
    TEST_CHECK(errorCode(loop.call("CancelEvent",
        {{"subscription", MsgPack(subscription)}})) == "SubscriptionInvalid");

    // 4. 无效 addr 订阅：AddrInvalid。
    TEST_CHECK(errorCode(loop.call("SubscribeEvent",
        {{"addr", MsgPack(std::uint64_t(999999))}, {"type", MsgPack("X")}})) == "AddrInvalid");

    // 5. 订阅 Released：对象 Release 后收到事件帧，channel 为空字符串。
    const MsgPack releasedSub = loop.call("SubscribeEvent",
        {{"addr", MsgPack(addr)}, {"type", MsgPack("Released")}});
    TEST_CHECK(releasedSub["ok"].bool_value());
    const std::size_t beforeRelease = loop.outbox.size();
    node->Release();
    TEST_CHECK(loop.outbox.size() == beforeRelease + 1);
    const MsgPack released = loop.lastFrame();
    TEST_CHECK(released["event"].string_value() == "Released");
    TEST_CHECK(static_cast<std::uint64_t>(released["addr"].int64_value()) == addr);
    TEST_CHECK(released["channel"].string_value().empty());

    // 6. Release 后该 addr 的操作回 AddrInvalid。
    TEST_CHECK(errorCode(loop.call("ReadData",
        {{"addr", MsgPack(addr)}, {"channel", MsgPack("Value")}})) == "AddrInvalid");

    // 7. 关闭连接后不再产生任何帧。
    loop.peer->Close();
    const std::size_t framesAfterClose = loop.outbox.size();
    counter->Increase();
    TEST_CHECK(loop.outbox.size() == framesAfterClose);

    delete node;  // owned=true，随节点释放 counter。
    return 0;
}
```

`tests/CMakeLists.txt` 追加：

```cmake
add_executable(test_runtime_bridge_protocol_event RuntimeBridgeProtocolEventTest.cpp)
target_link_libraries(test_runtime_bridge_protocol_event PRIVATE IObject::IObject)
target_include_directories(test_runtime_bridge_protocol_event PRIVATE ${CMAKE_SOURCE_DIR}/third_party/msgpack11)
add_test(NAME runtime_bridge_protocol_event COMMAND test_runtime_bridge_protocol_event)
```

- [ ] **Step 2: 构建验证失败**（UnknownOp）

- [ ] **Step 3: 实现**

`src/RuntimeBridgeProtocol.cpp` 两处改动：

1. dispatch 中 `} else if (op == "Close") {` 之前插入：

```cpp
        } else if (op == "SubscribeEvent") {
            handleSubscribeEvent(id, request);
        } else if (op == "CancelEvent") {
            handleCancelEvent(id, request);
```

2. `Impl` 中追加：

```cpp
    void handleSubscribeEvent(std::uint64_t id, const MsgPack& request) {
        std::uint64_t addr = 0;
        std::string type;
        if (!asUint64(request["addr"], addr) || addr == 0
            || !asString(request["type"], type) || type.empty()) {
            sendMessage(errorResponse(id, "MalformedMessage", "缺少合法的 addr 或 type 字段"));
            return;
        }
        if (!session->HasObject(addr)) {
            sendMessage(errorResponse(id, "AddrInvalid", "addr 无效或已失效"));
            return;
        }

        // 订阅 ID 在 SubscribeEvent 返回后才知道，回调经共享单元读取。
        auto subscriptionCell = std::make_shared<std::uint64_t>(0);
        const std::uint64_t subscription = session->SubscribeEvent(
            addr, type,
            [this, subscriptionCell](const RemoteEventMessage& message) {
                sendMessage(MsgPack(MsgPack::object{
                    {"event", MsgPack(message.type)},
                    {"subscription", MsgPack(*subscriptionCell)},
                    {"addr", MsgPack(message.source)},
                    {"channel", MsgPack(message.channel)}}));
            });
        if (subscription == 0) {
            sendMessage(errorResponse(id, "OperationFailed", "订阅失败"));
            return;
        }
        *subscriptionCell = subscription;
        subscriptions.insert(subscription);
        sendMessage(okResponse(id, {{"subscription", MsgPack(subscription)}}));
    }

    void handleCancelEvent(std::uint64_t id, const MsgPack& request) {
        std::uint64_t subscription = 0;
        if (!asUint64(request["subscription"], subscription) || subscription == 0) {
            sendMessage(errorResponse(id, "MalformedMessage", "缺少合法的 subscription 字段"));
            return;
        }
        if (subscriptions.erase(subscription) == 0) {
            sendMessage(errorResponse(id, "SubscriptionInvalid", "订阅无效或已取消"));
            return;
        }
        session->CancelEvent(subscription);
        sendMessage(okResponse(id));
    }
```

说明：对象 `Release` 时被内核取消的订阅仍残留在 `subscriptions` 集合中，之后 `CancelEvent` 会返回 ok（幂等简化，与 `RuntimeSubscription::Cancel` 语义一致）；这是第一版有意从简，会记录在规格的实现记录中。

- [ ] **Step 4: 构建并运行测试**

预期：11/11 通过（含 runtime_bridge_protocol_event）。

- [ ] **Step 5: Commit**

```bash
git add src/RuntimeBridgeProtocol.cpp tests/RuntimeBridgeProtocolEventTest.cpp tests/CMakeLists.txt
git commit -m "feat: add event subscription operations to bridge protocol"
```

---

### Task 7: 示例程序 10_MessagePackProtocolTest

**Files:**
- Create: `example/10_MessagePackProtocolTest.cpp`
- Modify: `example/CMakeLists.txt`

- [ ] **Step 1: 编写示例**

`example/10_MessagePackProtocolTest.cpp`（printf 演示风格，进程内回环展示协议往返）：

```cpp
// 10_MessagePackProtocolTest：演示 RuntimeBridgePeer 的 MessagePack 协议往返。
// 本示例在进程内回环：直接调用 ReceiveMessage 喂请求、在 SendCallback 里解析响应。
#include <iobject/Runtime.hpp>
#include <iobject/RuntimeBridge.hpp>
#include <iobject/RuntimeBridgeProtocol.hpp>
#include <iobject/RuntimeDomain.hpp>

#include <msgpack11.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using msgpack11::MsgPack;

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
        return true;
    }

private:
    bool on_ = false;
};

struct Loopback {
    iobject::RuntimeDomain domain;
    std::vector<std::string> outbox;
    std::unique_ptr<iobject::RuntimeBridgePeer> peer;
    std::uint64_t nextId = 1;

    Loopback()
        : peer(std::make_unique<iobject::RuntimeBridgePeer>(
              domain.BridgeRoot(), "MainScene",
              [this](iobject::ByteView frame) {
                  outbox.emplace_back(reinterpret_cast<const char*>(frame.data()), frame.size());
              })) {}

    MsgPack call(MsgPack::object object) {
        object.emplace(MsgPack("id"), MsgPack(nextId++));
        const std::string frame = MsgPack(std::move(object)).dump();
        peer->ReceiveMessage(
            iobject::ByteView(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()));
        std::string err;
        MsgPack response = MsgPack::parse(outbox.back(), err);
        if (!err.empty()) {
            std::printf("  解析响应失败: %s\n", err.c_str());
        }
        return response;
    }
};

} // namespace

int main() {
    Loopback loop;

    // 1. C++ 侧接入业务对象。
    iobject::IRuntimeObject* lamp = iobject::Runtime::make<Lamp>();
    loop.domain.RootAnchor()->Connect("Lamp", lamp);

    // 2. 握手：获得根锚点 addr。
    const MsgPack hello = loop.call({{"op", MsgPack("Connect")}, {"domain", MsgPack("MainScene")}});
    const std::uint64_t root = static_cast<std::uint64_t>(hello["root"].int64_value());
    std::printf("1. Connect ok=%d root=%llu\n", hello["ok"].bool_value() ? 1 : 0,
                static_cast<unsigned long long>(root));

    // 3. 逐级发现对象。
    const MsgPack found = loop.call(
        {{"op", MsgPack("GetChildItem")}, {"addr", MsgPack(root)}, {"childId", MsgPack("Lamp")}});
    const std::uint64_t lampAddr = static_cast<std::uint64_t>(found["addr"].int64_value());
    std::printf("2. GetChildItem childId=%s addr=%llu\n",
                found["childId"].string_value().c_str(),
                static_cast<unsigned long long>(lampAddr));

    // 4. 写入通道再读回。
    loop.call({{"op", MsgPack("WriteData")},
               {"addr", MsgPack(lampAddr)},
               {"channel", MsgPack("State")},
               {"data", MsgPack(MsgPack::binary{1})}});
    const MsgPack read = loop.call(
        {{"op", MsgPack("ReadData")}, {"addr", MsgPack(lampAddr)}, {"channel", MsgPack("State")}});
    std::printf("3. ReadData State -> %u\n",
                read["data"].is_binary() ? static_cast<unsigned>(read["data"].binary_items()[0]) : 0u);

    // 5. 订阅事件并发布。
    const MsgPack sub = loop.call({{"op", MsgPack("SubscribeEvent")},
                                   {"addr", MsgPack(lampAddr)},
                                   {"type", MsgPack("DataChannelChanged")}});
    std::printf("4. SubscribeEvent subscription=%llu\n",
                static_cast<unsigned long long>(sub["subscription"].int64_value()));
    lamp->Publish(iobject::RuntimeEventTypes::DataChannelChanged,
                  iobject::Runtime::make<iobject::DataChannelChangedEventData>("State"), true);
    std::string eventError;
    const MsgPack event = MsgPack::parse(loop.outbox.back(), eventError);
    if (!eventError.empty()) {
        std::printf("  解析事件帧失败: %s\n", eventError.c_str());
    }
    std::printf("5. 事件帧 event=%s channel=%s\n", event["event"].string_value().c_str(),
                event["channel"].string_value().c_str());

    // 6. 关闭。
    const MsgPack bye = loop.call({{"op", MsgPack("Close")}});
    std::printf("6. Close ok=%d IsOpen=%d\n", bye["ok"].bool_value() ? 1 : 0,
                loop.peer->IsOpen() ? 1 : 0);

    delete lamp;
    return 0;
}
```

示例不使用 TEST_CHECK，保持 printf 演示风格。

`example/CMakeLists.txt` 追加：

```cmake
add_executable(test_10_messagepack_protocol 10_MessagePackProtocolTest.cpp)
target_link_libraries(test_10_messagepack_protocol PRIVATE IObject::IObject)
target_include_directories(test_10_messagepack_protocol PRIVATE ${CMAKE_SOURCE_DIR}/third_party/msgpack11)
```

- [ ] **Step 2: 构建并运行示例**

```bash
cmake -S . -B build && cmake --build build --config Debug
./build/example/Debug/test_10_messagepack_protocol.exe
```

预期：输出 1–6，root 与 addr 非零，State 读回 1，事件帧 event=DataChannelChanged channel=State，Close 后 IsOpen=0。

- [ ] **Step 3: Commit**

```bash
git add example/10_MessagePackProtocolTest.cpp example/CMakeLists.txt
git commit -m "docs: add messagepack protocol loopback example"
```

---

### Task 8: 文档更新与最终验证

**Files:**
- Modify: `IObject_规则书与设计评估.md`
- Modify: `IObject_后续关键能力清单.md`
- Modify: `docs/superpowers/specs/2026-08-18-runtime-bridge-messagepack-protocol-design.md`

- [ ] **Step 1: 更新规则书**

a) 第 1 节头文件列表末尾追加：

```markdown
- `<iobject/RuntimeBridgeProtocol.hpp>`：MessagePack 协议适配器 `RuntimeBridgePeer`，把一个传输连接映射到一个 `RuntimeSession`（编解码使用 vendored 的 msgpack11，位于 `third_party/msgpack11/`）。
```

b) 第 5 节"上述五个头文件"改为"上述六个头文件"。

c) 第 6 节末尾追加一段：

```markdown
协议适配器 `RuntimeBridgePeer` 已实现：传输层每收到一条完整 MessagePack 消息就调用 `ReceiveMessage`，适配器把请求翻译成 `RuntimeSession` 调用并经 `SendCallback` 发回响应帧与事件帧。协议操作集为 `Connect`（握手，返回根锚点 addr）、`GetChildItem`（childId 单层名称，响应回显）、`ReadData`/`WriteData`、`SubscribeEvent`/`CancelEvent`、`Close`；对象标识 `addr` 即 `RemoteObjectHandle`。错误以 `{ok:false, error:{code, message}}` 返回，错误码见协议规格。MessagePack 编解码由 vendored 的 msgpack11 提供，真实传输层（WebSocket 等）仍未实现。
```

- [ ] **Step 2: 更新关键能力清单**

在"多运行时域与跨域引用（未实现）"一节的已实现段落末尾追加一句：

```markdown
MessagePack 协议适配器（`RuntimeBridgePeer`）已实现：定义了 Connect/GetChildItem/ReadData/WriteData/SubscribeEvent/CancelEvent/Close 操作与事件下行帧，编解码使用 vendored msgpack11；真实传输层与域名路由仍未实现。
```

- [ ] **Step 3: 更新协议规格（实现记录）**

在 `docs/superpowers/specs/2026-08-18-runtime-bridge-messagepack-protocol-design.md` 末尾追加一节：

```markdown
## 10. 实现记录

- 编解码库：msgpack11（`ar90n/msgpack11`，MIT），vendored 于 `third_party/msgpack11/`，随 IObject 静态库编译。
- 重复 `Connect`（会话已建立后再次握手）返回 `OperationFailed`，规格第 4.1 节未覆盖此情形，以本节为准。
- `childId` 为空或含 `.` 时返回 `MalformedMessage`（协议规定它是单层名称）。
- 对象 `Release` 后被内核自动取消的订阅，之后 `CancelEvent` 返回 ok（幂等从简）；`SubscriptionInvalid` 只覆盖从未存在或已被显式取消的 ID。
- 单条消息上限按建议值实现为 1 MiB，超过回 `MalformedMessage`（`id: 0`）并关闭连接。
- 事件帧 `channel` 字段仅 `DataChannelChanged` 非空；通用载荷快照（载荷对象的 `ReadData` 约定）是后续候选方向，当前未实现。
```

- [ ] **Step 4: 全量最终验证**

```bash
cmake -S . -B build && cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
./build/example/Debug/test_10_messagepack_protocol.exe
```

预期：11 个测试全部通过，示例输出正常。

- [ ] **Step 5: Commit**

```bash
git add IObject_规则书与设计评估.md IObject_后续关键能力清单.md docs/superpowers/specs/2026-08-18-runtime-bridge-messagepack-protocol-design.md
git commit -m "docs: document bridge protocol adapter"
```
