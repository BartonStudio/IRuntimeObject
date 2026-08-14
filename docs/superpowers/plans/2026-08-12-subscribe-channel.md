# SubscribeChannel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为同一运行时拓扑内的 `IRuntimeObject` 增加可取消的本地数据通道同步关系，并在成功同步后自动传播规范的通道变化事件。

**Architecture:** `RuntimeTopology` 保存独立于事件订阅的通道订阅记录，但两种记录共享 `RuntimeSubscription` 的 ID、取消和失效语义。`DataChannelChanged` 的规范载荷触发源通道读取与目标通道写入；专用的线程局部传播上下文为每个嵌套分支记录已访问节点，发现重复目标时终止该分支。

**Tech Stack:** C++20、CMake 3.20、静态库 `IObject`、现有演示可执行程序。

---

## 文件结构

- 修改 `include/iobject/IRuntimeObject.hpp`：声明两个成员 `SubscribeChannel` 重载、四参数公共函数，并补充精确的通道同步语义注释。
- 修改 `src/RuntimeObject.cpp`：实现通道订阅注册、按源/通道索引、统一取消和释放清理、传播上下文、自动读取/写入/通知，以及 pointer 转发。
- 创建 `example/08_SubscribeChannelTest.cpp`：中文输出展示同名同步、通道映射、公共函数、连续传播、失败路径、非规范通知、环截断、取消和释放。
- 修改 `example/CMakeLists.txt`：生成 `test_08_subscribe_channel`。
- 修改 `IObject_规则书与设计评估.md`、`IObject_后续关键能力清单.md`、`IObject_建议保留能力树.md`：将已实现的 `SubscribeChannel` 语义从未来设计改写为当前规则，并保留缓存、合并、异步队列、远程桥接为未来项。

项目既有示例是人工运行验证程序，用户已明确不使用 `assert`；本计划以构建并检查中文输出为验证方式，不新增单元测试框架。

### Task 1: 定义公开同步接口

**Files:**
- Modify: `include/iobject/IRuntimeObject.hpp:151-212`

- [ ] **Step 1: 在 `IRuntimeObject` 的 `SubscribeEvent` 后写入两个纯虚成员声明**

```cpp
/// 当前对象作为目标订阅 source 的同名数据通道；建立时不读取或写入数据。
virtual RuntimeSubscription SubscribeChannel(
    IRuntimeObject* source,
    DataChannelView channel) = 0;

/// 当前对象作为目标订阅 sourceChannel，并写入自身 targetChannel；建立时不读取或写入数据。
virtual RuntimeSubscription SubscribeChannel(
    IRuntimeObject* source,
    DataChannelView sourceChannel,
    DataChannelView targetChannel) = 0;
```

注释必须说明：空对象/通道、已释放节点和不同拓扑节点失败；传入 `IRuntimeObjectPointer` 只在建立时解引用；返回句柄析构、`Cancel()` 或任一端释放会解除关系。

- [ ] **Step 2: 在 `IRuntimeObject` 类定义后声明四参数公共函数**

```cpp
/// 建立 source 到 target 的数据通道同步；源与目标可使用不同通道名称。
RuntimeSubscription SubscribeChannel(
    IRuntimeObject* source,
    DataChannelView sourceChannel,
    IRuntimeObject* target,
    DataChannelView targetChannel);
```

- [ ] **Step 3: 构建库以验证新增纯虚接口会使实现暂时失败**

Run:

```bash
cmake --build build --config Debug --target IObject
```

Expected: 编译失败，报出 `RuntimeObject` 与 `RuntimeObjectPointer` 尚未实现新的纯虚 `SubscribeChannel` 重载。

- [ ] **Step 4: 不执行 Git 提交**

本工作区存在用户未提交变更，且当前任务没有要求 Git mutation。保留变更供后续任务继续使用。

### Task 2: 扩展拓扑的订阅注册表与统一句柄清理

**Files:**
- Modify: `src/RuntimeObject.cpp:112-151, 590-681`

- [ ] **Step 1: 在匿名命名空间的订阅记录区域定义通道订阅记录和索引键**

```cpp
struct ChannelSubscription {
    RuntimeObject* source = nullptr;
    DataChannel sourceChannel;
    RuntimeObject* target = nullptr;
    DataChannel targetChannel;
};

using ChannelSourceKey = std::pair<RuntimeObject*, DataChannel>;
```

- [ ] **Step 2: 扩展 `RuntimeTopology` 声明，使它能登记、查找和清理通道订阅**

向类添加以下成员函数：

```cpp
RuntimeSubscription subscribeChannel(
    IRuntimeObject* source,
    DataChannelView sourceChannel,
    IRuntimeObject* target,
    DataChannelView targetChannel);
void synchronizeChannels(RuntimeObject* source, const RuntimeObjectEvent& event);
void removeChannelSubscription(std::size_t id) noexcept;
void removeChannelSubscriptionsFor(RuntimeObject* object) noexcept;
```

向私有状态添加：

```cpp
std::map<std::size_t, ChannelSubscription> channelSubscriptionsById_;
std::map<ChannelSourceKey, std::set<std::size_t>> channelSubscriptionsBySource_;
std::map<RuntimeObject*, std::set<std::size_t>> channelSubscriptionsByTarget_;
```

继续使用现有 `nextSubscriptionId_`，保证事件与通道订阅 ID 在整个拓扑内唯一。

- [ ] **Step 3: 实现 `subscribeChannel` 的最小验证和索引写入**

实现顺序必须为：对 source、target 调用 `resolveRuntimeObject`；将结果 `dynamic_cast` 为 `RuntimeObject*`；拒绝空通道、非普通节点、非活动节点、不同 `topology()`；复制 `DataChannelView` 到 `DataChannel`；生成 ID；写入三个索引；返回 `detail::createRuntimeSubscription(this, id)`。

核心实现形状：

```cpp
const std::size_t id = nextSubscriptionId_++;
ChannelSubscription subscription{
    runtimeSource, DataChannel(sourceChannel), runtimeTarget, DataChannel(targetChannel)};
channelSubscriptionsById_.emplace(id, std::move(subscription));
channelSubscriptionsBySource_[{runtimeSource, DataChannel(sourceChannel)}].insert(id);
channelSubscriptionsByTarget_[runtimeTarget].insert(id);
return detail::createRuntimeSubscription(this, id);
```

保存局部 `DataChannel` 变量并在索引中复用它，避免依赖 `string_view` 生命周期。

- [ ] **Step 4: 使既有取消和有效性检查同时支持两类订阅**

将 `cancelSubscription` 改为先调用 `removeSubscription(id)`，再调用 `removeChannelSubscription(id)`；将 `isSubscriptionActive` 改为检查任意一个 `*ById_` 容器是否含该 ID。

`removeChannelSubscription` 必须从 `channelSubscriptionsById_` 取出完整记录，随后清理源键集合和目标集合；集合变空时删除对应 map 项。对不存在 ID 的调用直接返回。

- [ ] **Step 5: 使对象释放时清理事件与通道两类关系**

在 `removeSubscriptionsFor(RuntimeObject* object)` 的最后调用：

```cpp
removeChannelSubscriptionsFor(object);
```

`removeChannelSubscriptionsFor` 收集 source 为该对象的所有 ID 和 `channelSubscriptionsByTarget_[object]` 的 ID 到一个临时 `std::set<std::size_t>`，再逐一调用 `removeChannelSubscription`。先收集再删除，避免迭代期间失效。

- [ ] **Step 6: 构建静态库**

Run:

```bash
cmake --build build --config Debug --target IObject
```

Expected: 仍因未实现 `RuntimeObject` / `RuntimeObjectPointer` 的新增纯虚接口而失败；不应出现订阅索引类型、生命周期或编译警告错误。

### Task 3: 实现逐分支重复节点截断和实际同步

**Files:**
- Modify: `src/RuntimeObject.cpp:29-99, 618-631`

- [ ] **Step 1: 在现有事件派发上下文旁定义通道传播上下文**

```cpp
struct ChannelPropagationContext {
    std::vector<const RuntimeObject*> activePath;
};

thread_local ChannelPropagationContext* currentChannelPropagationContext = nullptr;
```

增加一个根作用域：仅在当前无上下文且事件是规范的 `DataChannelChanged` 时创建上下文并将事件源压入 `activePath`；退出时恢复先前线程局部指针。增加一个分支作用域：写入目标前检查 `activePath` 是否已经含目标，重复则向 `std::cerr` 输出包含“数据通道同步截断”“重复节点”的诊断并返回未进入；未重复时压入目标，析构时弹出。

分支作用域嵌套在同步调用周围。每个同步目标在当前路径进入、其 `Publish` 向下递归、返回后退出；因此同一源的多个直接目标各自从同一父路径分叉，互不污染。

- [ ] **Step 2: 在 `RuntimeTopology::publish` 中保留普通事件投递，并在其后调度通道同步**

实现必须保持当前“先按订阅 ID 快照调用普通事件处理器”的行为，随后调用：

```cpp
synchronizeChannels(source, event);
```

无匹配普通事件订阅时也不得提前 `return`，否则通道订阅会被错误跳过。将现有 `if (sourceFound == ...) return;` 改为仅在存在时处理事件处理器。

- [ ] **Step 3: 实现规范载荷筛选和源通道索引查找**

`RuntimeTopology::synchronizeChannels` 的开头应等价于：

```cpp
if (event.type != RuntimeEventTypes::DataChannelChanged || event.data == nullptr) {
    return;
}
const auto* changed = event.data->As<DataChannelChangedEventData>();
if (changed == nullptr) {
    return;
}
const auto found = channelSubscriptionsBySource_.find({source, changed->channel});
if (found == channelSubscriptionsBySource_.end()) {
    return;
}
```

复制 ID 集合到 `std::vector<std::size_t>` 后再迭代，允许回调或嵌套发布取消订阅。每次迭代均重新到 `channelSubscriptionsById_` 查询 ID；已取消、源/目标已失效的记录直接跳过。

- [ ] **Step 4: 为每条记录执行读取、写入和自动变化通知**

对每条有效记录，先进入目标的分支作用域；未进入即终止该分支。随后调用：

```cpp
bool received = false;
bool duplicateReceive = false;
bool writeSucceeded = false;
const bool readSucceeded = subscription.source->ReadData(
    subscription.sourceChannel,
    [&](ByteView bytes) {
        if (received) {
            duplicateReceive = true;
            return;
        }
        received = true;
        writeSucceeded = subscription.target->WriteData(subscription.targetChannel, bytes);
    });
```

仅当 `readSucceeded && received && !duplicateReceive && writeSucceeded` 为真时调用目标的内部 `publishEvent`，事件载荷使用：

```cpp
std::unique_ptr<IRuntimeObject> data(
    Runtime::make<DataChannelChangedEventData>(subscription.targetChannel));
subscription.target->publishEvent(
    {RuntimeEventType(RuntimeEventTypes::DataChannelChanged), subscription.target, data.get()});
```

这里的 `ByteView` 只在 `ReadData` 的 receiver 内传给 `WriteData`，不保存指针或视图，不引入缓存或深拷贝。读取器违反“恰好调用一次”的契约时，第二次调用不再写入，且第一次写入不继续发布下游变化。

- [ ] **Step 5: 构建静态库**

Run:

```bash
cmake --build build --config Debug --target IObject
```

Expected: 通道同步内部实现编译通过；唯一剩余失败应是 `RuntimeObject` / `RuntimeObjectPointer` 的纯虚接口缺失。

### Task 4: 连接公开接口和透明指针转发

**Files:**
- Modify: `src/RuntimeObject.cpp:158-201, 394-458, 713-725`

- [ ] **Step 1: 在 `RuntimeObject` 实现两个成员重载**

```cpp
RuntimeSubscription SubscribeChannel(
    IRuntimeObject* source,
    DataChannelView channel) override {
    return SubscribeChannel(source, channel, channel);
}

RuntimeSubscription SubscribeChannel(
    IRuntimeObject* source,
    DataChannelView sourceChannel,
    DataChannelView targetChannel) override {
    return state_ == RuntimeObjectState::Active
        ? topology_->subscribeChannel(source, sourceChannel, this, targetChannel)
        : RuntimeSubscription();
}
```

- [ ] **Step 2: 在 `RuntimeObjectPointer` 转发两个成员重载**

```cpp
RuntimeSubscription SubscribeChannel(
    IRuntimeObject* source,
    DataChannelView channel) override {
    return target_ ? target_->SubscribeChannel(source, channel) : RuntimeSubscription();
}

RuntimeSubscription SubscribeChannel(
    IRuntimeObject* source,
    DataChannelView sourceChannel,
    DataChannelView targetChannel) override {
    return target_ ? target_->SubscribeChannel(source, sourceChannel, targetChannel)
                   : RuntimeSubscription();
}
```

这样 pointer 只在调用建立订阅的那一刻转发，不保留或迁移任何关系。

- [ ] **Step 3: 在 `namespace iobject` 的工厂函数旁实现公共四参数函数**

```cpp
RuntimeSubscription SubscribeChannel(
    IRuntimeObject* source,
    DataChannelView sourceChannel,
    IRuntimeObject* target,
    DataChannelView targetChannel) {
    return target == nullptr
        ? RuntimeSubscription()
        : target->SubscribeChannel(source, sourceChannel, targetChannel);
}
```

目标为 pointer 时由其成员函数在本次调用转发；目标为空返回失效句柄。

- [ ] **Step 4: 构建库和既有示例**

Run:

```bash
cmake --build build --config Debug --target IObject test_03_event test_05_data_channel test_06_data_channel_change test_07_pointer
```

Expected: 所有目标生成成功；已有事件、数据通道和 pointer 示例行为不变。

### Task 5: 新增人工验证示例

**Files:**
- Create: `example/08_SubscribeChannelTest.cpp`
- Modify: `example/CMakeLists.txt:19-20`

- [ ] **Step 1: 将 `CounterState` 示例类写入新示例**

定义 `CounterState`，具备 `std::string name_`、`std::int32_t state_`、支持 `"State"` 与 `"Replica"` 的 `ReadData` / `WriteData`。使用四字节小端编码；每次成功 `WriteData` 输出对象名、通道名和解码值，但不自行发布事件。提供 `GetState()` 和 `GetReplica()` 供示例打印最终值。

- [ ] **Step 2: 编写示例的对象与订阅关系**

示例必须用显式类型而非 `auto`，保存所有 `RuntimeSubscription`，并依次建立：

```cpp
IRuntimeObject* objectA = Runtime::make<CounterState>("A", 10);
IRuntimeObject* objectB = Runtime::make<CounterState>("B", 0);
IRuntimeObject* objectC = Runtime::make<CounterState>("C", 0);
IRuntimeObject* objectD = Runtime::make<CounterState>("D", 0);

RuntimeSubscription sameName = objectB->SubscribeChannel(objectA, "State");
RuntimeSubscription mapped = objectC->SubscribeChannel(objectA, "State", "Replica");
RuntimeSubscription publicFunction = iobject::SubscribeChannel(objectB, "State", objectD, "State");
```

通过 `objectA->WriteData("State", ...)` 后发布规范的 `DataChannelChanged("State")`。输出应展示 B 收到 `State`、C 收到 `Replica`、D 经 B 的自动通知收到 `State`。

- [ ] **Step 3: 写入错误和截断场景**

在同一程序继续演示：

```cpp
objectA->Publish(RuntimeEventTypes::DataChannelChanged, nullptr);
objectA->Publish(RuntimeEventTypes::DataChannelChanged,
                 Runtime::make<ChildEventData>("not-channel", objectB), true);
objectA->Publish(RuntimeEventTypes::DataChannelChanged,
                 Runtime::make<DataChannelChangedEventData>("Other"), true);
```

上述三次均不应产生通道写入。再建立 `objectA -> objectB -> objectC -> objectB` 的 `State` 关系，发布 A 的变化；输出应包含一次“重复节点”截断诊断，且 C 之后不会再次写入 B。取消 `sameName` 后再次发布，输出应显示 B 不再接收 A 的直接同步。最后释放一个订阅端并打印对应句柄 `IsActive()` 为 `false`。

- [ ] **Step 4: 在 CMake 示例列表增加目标**

在 `example/CMakeLists.txt` 末尾增加：

```cmake
add_executable(test_08_subscribe_channel 08_SubscribeChannelTest.cpp)
target_link_libraries(test_08_subscribe_channel PRIVATE IObject::IObject)
```

- [ ] **Step 5: 构建并运行新示例**

Run:

```bash
cmake --build build --config Debug --target test_08_subscribe_channel
build/example/Debug/test_08_subscribe_channel.exe
```

Expected: 构建成功；中文输出展示三种接口、同步传播、三种不可识别通知被忽略、重复节点截断、取消和释放自动解绑。程序退出码为 `0`。

### Task 6: 更新规则书与能力清单

**Files:**
- Modify: `IObject_规则书与设计评估.md:95-142, 157-163, 192`
- Modify: `IObject_后续关键能力清单.md:39-59`
- Modify: `IObject_建议保留能力树.md:47-52`

- [ ] **Step 1: 在规则书的数据通道章节记录已实现接口与传播规则**

加入三种 `SubscribeChannel` 签名和以下不可省略的规则：建立不进行初始同步；仅规范 `DataChannelChangedEventData` 且通道精确匹配时同步；成功读取和写入后框架发布目标变化；普通 `WriteData` 不自动发布；读取、写入失败或不规范载荷静默停止该分支；`RuntimeSubscription` 管理关系；指针只在建立时解引用。

- [ ] **Step 2: 在规则书记录节点重复截断语义**

明确每条传播分支存储已访问节点，重复目标不写入且终止当前分支；`A -> B -> C -> B -> E` 在第二次进入 B 时停止，E 不执行；兄弟分支互不影响；保留事件深度 32 与总发布数 128 的全局保险。

- [ ] **Step 3: 保持未来能力边界清晰**

缓存、合并、异步队列和远程传输章节继续标为未实现。远程章节改为使用 `SubscribeChannel` 作为本地基础能力，但不声称已实现远程代理、稳定对象 ID、连接桥接或网络协议。

- [ ] **Step 4: 同步两份能力清单**

将“当前仅 `SubscribeEvent`”改为“`SubscribeEvent` 与 `SubscribeChannel` 均返回 `RuntimeSubscription`”。列出通道同步的源/目标通道映射、自动传播、无初始快照、重复节点截断和自动解绑；不要把缓存、队列或远程功能标为已实现。

- [ ] **Step 5: 检查文档和 API 名称一致性**

Run:

```bash
rg -n "ChannelSync|SubscribeChannel|DataChannelChangedEventData|AddEventHandler" IObject_规则书与设计评估.md IObject_后续关键能力清单.md IObject_建议保留能力树.md include example src
```

Expected: `SubscribeChannel` 叙述与实际 API 一致；不再把 `ChannelSync` 写为当前独立类；无过期 `AddEventHandler` 文字。

### Task 7: 全量验证

**Files:**
- Verify only: `CMakeLists.txt`, `example/CMakeLists.txt`, `include/iobject/*.hpp`, `src/RuntimeObject.cpp`, `example/*.cpp`

- [ ] **Step 1: 配置并构建整个 Debug 工程**

Run:

```bash
cmake -S . -B build
cmake --build build --config Debug
```

Expected: `IObject.lib` 和全部 `test_01_*` 至 `test_08_subscribe_channel` 目标构建成功。若环境继续输出 `pwsh.exe` 缺失提示，记录为现有非阻断环境提示，前提是 CMake 返回成功。

- [ ] **Step 2: 运行关键回归和新示例**

Run:

```bash
for exe in build/example/Debug/test_03_event.exe build/example/Debug/test_05_data_channel.exe build/example/Debug/test_06_data_channel_change.exe build/example/Debug/test_07_pointer.exe build/example/Debug/test_08_subscribe_channel.exe; do "$exe" || exit 1; done
```

Expected: 所有程序退出码为 `0`；事件、数据通道和 pointer 旧功能未回归；新示例按设计展示同步、截断与解绑。

- [ ] **Step 3: 检查最终改动范围**

Run:

```bash
git diff --check
git diff -- include/iobject/IRuntimeObject.hpp src/RuntimeObject.cpp example/CMakeLists.txt example/08_SubscribeChannelTest.cpp IObject_规则书与设计评估.md IObject_后续关键能力清单.md IObject_建议保留能力树.md
```

Expected: 无空白错误；差异仅包含本计划对应的接口、实现、示例与文档更新，以及工作区原有的用户变更。

- [ ] **Step 4: 不执行 Git 提交或推送**

除非用户在执行阶段明确要求，保持当前工作区变更未提交。
