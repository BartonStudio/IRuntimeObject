# PublishEvent 与 PublishChannel 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将通用事件发布接口统一命名为 `PublishEvent`，并提供用于标准数据通道变更通知的成员与自由函数 `PublishChannel`。

**Architecture:** `PublishEvent` 继续是携带任意事件名、可选载荷和载荷所有权策略的低层事件入口；`PublishChannel` 是其上层固定封装，只说明指定数据通道已发生业务确认的逻辑变化。普通包装对象负责构造标准 `DataChannelChangedEventData` 载荷并复用已有事件派发路径；透明指针只将调用转发给当前绑定目标，自由函数只处理空指针后委托成员函数。

**Tech Stack:** C++20、CMake、MSVC 静态库 `IObject`、现有中文示例程序。

---

## 文件结构与职责

| 文件 | 修改类型 | 本次职责 |
|---|---|---|
| `include/iobject/IRuntimeObject.hpp` | 修改 | 公开 `PublishEvent` / `PublishChannel` 接口、自由函数声明及相关中文注释。 |
| `src/RuntimeObject.cpp` | 修改 | 普通对象和透明指针的具体实现、通道同步成功后的标准通知、自由函数实现。 |
| `example/03_EventTest.cpp` | 修改 | 将旧事件发布调用改为 `PublishEvent`，保持事件与载荷生命周期演示。 |
| `example/06_DataChannelChangeTest.cpp` | 修改 | 用成员 `PublishChannel` 演示标准通道变化通知。 |
| `example/07_PointerTest.cpp` | 修改 | 演示透明指针对 `PublishEvent` 与 `PublishChannel` 的转发，及空指针静默行为。 |
| `example/08_SubscribeChannelTest.cpp` | 修改 | 删除手工构造标准通道事件的本地 helper，演示成员和自由函数入口，并保留低层错误事件不会触发通道同步的案例。 |
| `IObject_规则书与设计评估.md` | 修改 | 明确三层操作 `WriteData`、`PublishChannel`、`PublishEvent` 的边界与载荷所有权。 |
| `IObject_建议保留能力树.md` | 修改 | 将标准通道变化能力表述更新为 `PublishChannel`，将旧 `Publish` 改为 `PublishEvent`。 |
| `IObject_后续关键能力清单.md` | 修改 | 将未来规划中的旧事件名称替换为 `PublishEvent`，记录标准入口与低层手工事件的边界。 |

本工程没有 `CTest` 注册的断言式单元测试。`example/` 中的八个可执行程序是当前约定的可运行验证与 API 用法展示；本计划不引入测试框架或 `assert`，而以编译和检查中文控制台输出验证行为。

> 所有任务均不得执行 `git commit`、`git push`、`git reset`、`git rebase` 或其他 Git 修改命令：当前用户未在本次实施中授权 Git mutation。每个检查点仅检查工作区内容和构建结果。

### Task 1: 先改示例，建立 API 调用契约

**Files:**
- Modify: `example/08_SubscribeChannelTest.cpp:80-155`
- Test: `example/08_SubscribeChannelTest.cpp`

- [ ] **Step 1: 删除本地手工通道事件 helper，改为新 API 调用。**

删除当前局部函数：

```cpp
void PublishChannelChanged(IRuntimeObject* source, const char* channel) {
    source->Publish(RuntimeEventTypes::DataChannelChanged,
                    Runtime::make<DataChannelChangedEventData>(channel), true);
}
```

将成功触发通道同步的调用改成以下两种入口：

```cpp
objectA->PublishChannel("State");
iobject::PublishChannel(objectA, "State");
```

保留故意发送错误事件的验证，但改用新低层 API：

```cpp
objectA->PublishEvent("OtherEvent", nullptr);
objectA->PublishEvent(RuntimeEventTypes::DataChannelChanged,
                      Runtime::make<HealthData>(50), true);
```

在示例末尾添加不会导致写入的静默误调用：

```cpp
objectA->PublishChannel("");
iobject::PublishChannel(nullptr, "State");
std::cout << "空通道与空对象的 PublishChannel 已静默忽略。\n";
```

- [ ] **Step 2: 配置并构建，确认新契约在实现前不可编译。**

Run:

```bash
cmake -S . -B build
cmake --build build --config Debug
```

Expected: 构建失败，错误明确指出 `IRuntimeObject` 尚无 `PublishChannel` 成员、命名空间 `iobject` 尚无 `PublishChannel` 自由函数，且旧 `Publish` 名称仍待改为 `PublishEvent`。这证明示例已精确表达目标 API，而不是继续依赖手工 payload helper。

- [ ] **Step 3: 检查工作区差异，不执行 Git 提交。**

Run:

```bash
git diff -- example/08_SubscribeChannelTest.cpp
git diff --check
```

Expected: helper 被完整移除；成功同步路径使用 `PublishChannel`；故意无效的低层事件使用 `PublishEvent`；没有空白错误。

### Task 2: 定义公开事件与通道通知接口

**Files:**
- Modify: `include/iobject/IRuntimeObject.hpp:180-235`
- Test: `example/08_SubscribeChannelTest.cpp`

- [ ] **Step 1: 将通用事件纯虚接口彻底重命名为 `PublishEvent`。**

将：

```cpp
virtual void Publish(RuntimeEventTypeView type, IRuntimeObject* data = nullptr,
                     bool destroyDataAfterPublish = false) = 0;
```

替换为：

```cpp
virtual void PublishEvent(RuntimeEventTypeView type, IRuntimeObject* data = nullptr,
                          bool destroyDataAfterPublish = false) = 0;
```

在其上方保留并更新注释，说明：`data` 可为空；`destroyDataAfterPublish == true` 时，框架会在本次调用结束时接管并析构 `data`；该接口是通用低层事件发布入口。

- [ ] **Step 2: 紧随 `PublishEvent` 声明添加标准通道通知纯虚接口。**

添加以下完整声明与中文行为注释：

```cpp
/// 通知当前对象：指定数据通道已发生业务确认的逻辑变化。
/// 此操作不会读取、写入或比较通道数据，只发布标准的 DataChannelChanged 事件。
/// 空通道、已 Release 的对象以及未绑定的透明指针均静默忽略。
virtual void PublishChannel(DataChannelView channel) = 0;
```

- [ ] **Step 3: 更新 `WriteData` 注释，并声明自由函数入口。**

将 `WriteData` 的后续通知提示改成：

```cpp
/// WriteData 不会自动发布 DataChannelChanged；业务确认数据已变化后，
/// 应自行调用 PublishChannel(channel)。
```

在类定义后的现有自由 `SubscribeChannel` 声明附近添加：

```cpp
/// 空 source 时静默忽略；其他行为与 source->PublishChannel(channel) 相同。
void PublishChannel(IRuntimeObject* source, DataChannelView channel);
```

- [ ] **Step 4: 执行仅编译接口所需的构建检查。**

Run:

```bash
cmake --build build --config Debug
```

Expected: 构建仍会失败，但错误从“接口不存在”前移为 `RuntimeObject` 和 `RuntimeObjectPointer` 未实现新的纯虚 `PublishEvent` / `PublishChannel`。这证明公开头文件声明已生效且实现端必须同步完成。

- [ ] **Step 5: 检查头文件 API 形状，不执行 Git 提交。**

Run:

```bash
grep -n "PublishEvent\|PublishChannel\|virtual void Publish(" include/iobject/IRuntimeObject.hpp
git diff --check
```

Expected: 存在 `PublishEvent`、成员 `PublishChannel` 和自由 `PublishChannel`；头文件中不再存在 `virtual void Publish(`；没有格式错误。

### Task 3: 实现普通 RuntimeObject 的两层发布入口

**Files:**
- Modify: `src/RuntimeObject.cpp:285-305`
- Test: `example/06_DataChannelChangeTest.cpp`, `example/08_SubscribeChannelTest.cpp`

- [ ] **Step 1: 将普通对象实现重命名为 `PublishEvent`，不改变所有权和派发逻辑。**

将实现签名从：

```cpp
void Publish(RuntimeEventTypeView type, IRuntimeObject* data, bool destroyDataAfterPublish) override {
```

替换为：

```cpp
void PublishEvent(RuntimeEventTypeView type, IRuntimeObject* data,
                  bool destroyDataAfterPublish) override {
```

保留现有的异常安全所有权接管和状态检查逻辑：

```cpp
std::unique_ptr<IRuntimeObject> ownedData(destroyDataAfterPublish ? data : nullptr);
if (state_ != ReleaseState::Active || type.empty()) {
    return;
}
publishEvent({RuntimeEventType(type), this, data});
```

私有 helper `publishEvent(...)` 保持小写，不重命名；它是内部实际派发路径，不是公开 API。

- [ ] **Step 2: 在 `PublishEvent` 后实现 `PublishChannel`。**

添加以下实现；先检查状态和通道，避免无效调用时分配载荷：

```cpp
void PublishChannel(DataChannelView channel) override {
    if (state_ != ReleaseState::Active || channel.empty()) {
        return;
    }

    std::unique_ptr<IRuntimeObject> data(
        Runtime::make<DataChannelChangedEventData>(DataChannel(channel)));
    PublishEvent(RuntimeEventTypes::DataChannelChanged, data.release(), true);
}
```

这里必须显式构造 `DataChannel(channel)`，不得依赖 `std::string_view` 到 `std::string` 的隐式转换。`PublishEvent(..., true)` 已用局部 `std::unique_ptr` 接管载荷，因此派发正常返回、无订阅者、回调异常离开等路径都不会泄漏这个框架创建的标准载荷。

- [ ] **Step 3: 构建，确认普通对象实现已满足自己的纯虚接口。**

Run:

```bash
cmake --build build --config Debug
```

Expected: 构建可能仍因透明指针尚未实现 `PublishEvent` / `PublishChannel` 而失败；错误不得再指向普通 `RuntimeObject` 缺少这两个纯虚函数实现。透明指针完成后，再在 Task 6 运行 `test_06_data_channel_change.exe`，它应输出 `State` 与 `Metadata` 通道变化对应的中文说明，且不需要业务代码手工创建 `DataChannelChangedEventData`。

- [ ] **Step 4: 审查普通对象实现的职责边界。**

Run:

```bash
grep -n -A18 -B3 "void PublishEvent\|void PublishChannel" src/RuntimeObject.cpp
git diff --check
```

Expected: `PublishChannel` 没有 `ReadData`、`WriteData`、旧值比较或字节处理；唯一事件类型为 `RuntimeEventTypes::DataChannelChanged`；没有格式错误。

### Task 4: 实现透明指针与自由函数委托

**Files:**
- Modify: `src/RuntimeObject.cpp:545-570,1070-1090`
- Test: `example/07_PointerTest.cpp`, `example/08_SubscribeChannelTest.cpp`

- [ ] **Step 1: 将透明指针的通用发布实现改名为 `PublishEvent`。**

将透明指针实现替换为：

```cpp
void PublishEvent(RuntimeEventTypeView type, IRuntimeObject* data,
                  bool destroyDataAfterPublish) override {
    if (target_ != nullptr) {
        target_->PublishEvent(type, data, destroyDataAfterPublish);
    } else if (destroyDataAfterPublish) {
        delete data;
    }
}
```

必须保留未绑定透明指针且 `destroyDataAfterPublish == true` 时析构 `data` 的行为。调用方已将所有权交给框架，未绑定不能导致载荷泄漏。

- [ ] **Step 2: 添加透明指针的 `PublishChannel` 转发实现。**

在上述函数后添加：

```cpp
void PublishChannel(DataChannelView channel) override {
    if (target_ != nullptr) {
        target_->PublishChannel(channel);
    }
}
```

不要在透明指针内部构造载荷、检查目标 Release 状态或复制普通对象逻辑；目标对象自己的实现负责这些规则。未绑定透明指针直接返回，符合静默无操作要求。

- [ ] **Step 3: 在自由 `SubscribeChannel` 函数相邻处实现自由 `PublishChannel`。**

添加：

```cpp
void PublishChannel(IRuntimeObject* source, DataChannelView channel) {
    if (source != nullptr) {
        source->PublishChannel(channel);
    }
}
```

不得绕过成员函数直接访问拓扑、事件派发器或透明指针内部状态。

- [ ] **Step 4: 为 `07_PointerTest.cpp` 加入可观察的通道转发演示。**

在已有 `first`、透明 `pointer` 和事件订阅上下文中，订阅 `first` 的标准通道事件并输出来源和通道：

```cpp
RuntimeSubscription channelSubscription = first->SubscribeEvent(
    RuntimeEventTypes::DataChannelChanged,
    [](const RuntimeObjectEvent& event) {
        const DataChannelChangedEventData* changed = event.data != nullptr
            ? event.data->As<DataChannelChangedEventData>()
            : nullptr;
        if (changed != nullptr) {
            std::cout << "透明指针转发通道通知："
                      << changed->channel << "，来源仍为绑定对象。\n";
        }
    });

pointer->PublishChannel("Value");
IRuntimeObject* emptyPointer = Runtime::makePointer();
emptyPointer->PublishChannel("Value");
std::cout << "未绑定透明指针的通道通知已静默忽略。\n";
```

按该示例现有生命周期规则，在程序结束前对 `emptyPointer` 与 `pointer` 执行 `Release()` 后 `delete`，并保持已有对象的释放顺序正确。回调不得使用 `assert`。

- [ ] **Step 5: 配置、构建并运行指针和通道订阅示例。**

Run:

```bash
cmake --build build --config Debug
./build/example/Debug/test_07_pointer.exe
./build/example/Debug/test_08_subscribe_channel.exe
```

Expected: 构建成功；`test_07_pointer.exe` 输出透明指针发布后由绑定对象作为事件来源的通道通知，并输出未绑定指针被忽略的说明；`test_08_subscribe_channel.exe` 能运行到结束，成员和自由 `PublishChannel` 都能驱动既有同步逻辑。

- [ ] **Step 6: 检查透明指针所有权与自由函数没有重复逻辑。**

Run:

```bash
grep -n -A15 -B3 "void PublishEvent\|void PublishChannel(IRuntimeObject" src/RuntimeObject.cpp
git diff --check
```

Expected: 透明指针的通用发布仍对未绑定且已转移所有权的 data 执行 `delete`；自由函数只含空指针判断和一次成员调用；没有格式错误。

### Task 5: 用标准封装完成通道同步的后续传播

**Files:**
- Modify: `src/RuntimeObject.cpp:895-915`
- Test: `example/08_SubscribeChannelTest.cpp`

- [ ] **Step 1: 替换同步成功后的手工 payload 创建与低层发布。**

在 `RuntimeTopology::synchronizeChannels` 的 `ReadData`、重复链路检查和 `WriteData` 成功之后，将：

```cpp
std::unique_ptr<IRuntimeObject> payload(
    Runtime::make<DataChannelChangedEventData>(subscription.targetChannel));
subscription.target->Publish(
    RuntimeEventTypes::DataChannelChanged, payload.release(), true);
```

替换为：

```cpp
subscription.target->PublishChannel(subscription.targetChannel);
```

保留其前所有判断，包括读取失败、目标无效、链路重复节点检测，以及在单个订阅处理完成后继续下一个订阅的既有控制流。

- [ ] **Step 2: 构建并执行同步链路演示。**

Run:

```bash
cmake --build build --config Debug
./build/example/Debug/test_08_subscribe_channel.exe
```

Expected: `State` 的变化仍从源对象向订阅目标传播；目标写入成功后能以标准事件继续向下游传播；错误事件名、错误载荷类型或错误通道不会被通道同步识别为有效更新。

- [ ] **Step 3: 审查同步实现只复用公开标准入口。**

Run:

```bash
grep -n -A65 "void RuntimeTopology::synchronizeChannels" src/RuntimeObject.cpp
git diff --check
```

Expected: 同步成功分支只调用 `subscription.target->PublishChannel(subscription.targetChannel)`，没有在该函数中手工构造 `DataChannelChangedEventData`；已有事件派发链路保护仍在原位置。

### Task 6: 统一所有示例的事件与通道用法

**Files:**
- Modify: `example/03_EventTest.cpp:70-115`
- Modify: `example/06_DataChannelChangeTest.cpp:120-150`
- Modify: `example/07_PointerTest.cpp:60-115`
- Modify: `example/08_SubscribeChannelTest.cpp:80-160`
- Test: `example/03_EventTest.cpp`, `example/06_DataChannelChangeTest.cpp`, `example/07_PointerTest.cpp`, `example/08_SubscribeChannelTest.cpp`

- [ ] **Step 1: 将事件演示中的所有旧 `Publish` 调用改为 `PublishEvent`。**

在 `03_EventTest.cpp` 中，将每个：

```cpp
objectA->Publish("CustomEvent", data, true);
```

形式改为：

```cpp
objectA->PublishEvent("CustomEvent", data, true);
```

对 `RuntimeEventTypes::Released`、普通字符串事件和栈上载荷的 `Publish` 调用都执行同样替换。控制台说明中的“Publish 返回后”改为“PublishEvent 返回后”，以免示例文字与公开 API 不一致。

在 `07_PointerTest.cpp` 中，将：

```cpp
pointer->Publish("Changed");
```

改为：

```cpp
pointer->PublishEvent("Changed");
```

- [ ] **Step 2: 用成员 `PublishChannel` 简化 `06_DataChannelChangeTest.cpp`。**

移除不再使用的：

```cpp
using iobject::DataChannelChangedEventData;
```

将手工标准事件发布替换为：

```cpp
objectB->PublishChannel("State");
objectB->PublishChannel("Metadata");
```

示例继续由业务先更新被包装对象的状态，再明确通知通道变化；不得暗示 `WriteData` 会自动通知。

- [ ] **Step 3: 完成 `08_SubscribeChannelTest.cpp` 的两种入口和静默边界演示。**

确保至少一次传播从成员入口触发：

```cpp
objectA->PublishChannel("State");
```

确保至少一次传播从自由函数触发：

```cpp
iobject::PublishChannel(objectA, "State");
```

保留以下低层错误事件，以演示并验证同步只接受正确的标准事件：

```cpp
objectA->PublishEvent("OtherEvent", nullptr);
objectA->PublishEvent(RuntimeEventTypes::DataChannelChanged,
                      Runtime::make<HealthData>(50), true);
```

空通道和空 source 调用必须只输出“已静默忽略”的说明，不能调用目标的 `WriteData`。

- [ ] **Step 4: 构建并逐个运行全部现有示例。**

Run:

```bash
cmake -S . -B build
cmake --build build --config Debug
./build/example/Debug/test_01_wrapping.exe
./build/example/Debug/test_02_topology.exe
./build/example/Debug/test_03_event.exe
./build/example/Debug/test_04_type_conversion.exe
./build/example/Debug/test_05_data_channel.exe
./build/example/Debug/test_06_data_channel_change.exe
./build/example/Debug/test_07_pointer.exe
./build/example/Debug/test_08_subscribe_channel.exe
```

Expected: 八个程序均正常退出。`test_03_event.exe` 文字仅使用 `PublishEvent`；`test_06_data_channel_change.exe` 显示 `State` 与 `Metadata` 的标准通知；`test_07_pointer.exe` 显示指针转发和空指针静默；`test_08_subscribe_channel.exe` 显示成员和自由函数入口均可启动同步，错误事件不会触发同步。构建环境若打印既有 `pwsh.exe` 缺失警告，只要构建命令退出码为 0 且八个 exe 均已生成和正常退出，即不视为本次改动失败。

- [ ] **Step 5: 扫描示例，确认没有旧 API 或手工标准 helper。**

Run:

```bash
grep -R -n -E "\bPublish\s*\(|PublishChannelChanged" example
grep -R -n -E "\bPublishEvent\s*\(|\bPublishChannel\s*\(" example
git diff --check
```

Expected: 第一条无输出；第二条列出本次改写的示例调用；没有格式错误。

### Task 7: 更新规则书和能力文档

**Files:**
- Modify: `IObject_规则书与设计评估.md:105-200`
- Modify: `IObject_建议保留能力树.md:40-70`
- Modify: `IObject_后续关键能力清单.md:35-80`
- Test: 三份根目录 Markdown 文档

- [ ] **Step 1: 在规则书中明确数据写入、通道通知和通用事件的边界。**

在 `IObject_规则书与设计评估.md` 的数据通道段，将手工构造标准载荷的示例替换为：

```cpp
object->WriteData("State", input);
object->PublishChannel("State");
```

添加以下规则文字：

```markdown
- `WriteData(channel, input)` 只向对象提交输入；对象如何解释和处理输入由被包装类型的 `WriteData` 逻辑决定，框架不会自动发布事件。
- `PublishChannel(channel)` 表示业务已经确认该通道发生逻辑变化。它不读取、不写入、不比较数据；框架内部创建 `DataChannelChangedEventData`，并发布 `RuntimeEventTypes::DataChannelChanged`。
- 自由函数 `iobject::PublishChannel(source, channel)` 与成员调用语义相同；`source == nullptr` 时静默忽略。
- 空通道、已 Release 对象、未绑定透明指针和透明指针的已 Release 绑定目标均静默忽略，不抛异常也不发布事件。
- 通用事件应使用 `PublishEvent(type, data, destroyDataAfterPublish)`。只有它允许业务传入自定义事件与自定义载荷；`destroyDataAfterPublish == true` 时本次调用接管并析构载荷。
- `PublishChannel` 创建的标准载荷始终由框架接管，调用方不应也不需要手动 `delete`。
```

将全文中用于通用事件的旧 `Publish(...)` 文字、代码和所有权说明改为 `PublishEvent(...)`，包括 `Released` 等非通道事件的示例。

- [ ] **Step 2: 更新能力树的标准名称与所有权描述。**

在 `IObject_建议保留能力树.md` 中，将“通道变化通知”统一表述为：

```markdown
- 业务在确认数据通道变化后调用 `PublishChannel(channel)`，框架发布标准 `DataChannelChanged` 事件；通道同步可据此识别并传播。
```

将所有通用事件发布的旧名称改为 `PublishEvent`，并补充：传入 `destroyDataAfterPublish == true` 的载荷由该调用接管；`PublishChannel` 的标准载荷由框架内部创建与销毁。

- [ ] **Step 3: 更新后续关键能力清单中的低层与标准入口边界。**

在 `IObject_后续关键能力清单.md` 中，将旧 `Publish` 统一改名为 `PublishEvent`，并将规范通道事件说明扩展为：

```markdown
- 推荐业务通过 `PublishChannel(channel)` 发送通道变化；它保证事件名和 `DataChannelChangedEventData` 载荷的标准形状。
- `PublishEvent(RuntimeEventTypes::DataChannelChanged, data, ...)` 仍是低层通用能力，但业务手工构造的事件可能具有错误载荷类型或通道，通道同步只能识别正确的标准事件。
```

保留文档中既有的缓存、合并、异步队列和远程传输等未来规划内容，不将其实现承诺为本次代码范围。

- [ ] **Step 4: 审查规则文档，排除旧 API 名称。**

Run:

```bash
grep -n -E "\bPublish\s*\(" IObject_规则书与设计评估.md IObject_建议保留能力树.md IObject_后续关键能力清单.md
grep -n -E "PublishEvent|PublishChannel|WriteData" IObject_规则书与设计评估.md IObject_建议保留能力树.md IObject_后续关键能力清单.md
git diff --check
```

Expected: 第一条无输出；第二条显示三层职责与新名称的文档记录；没有 Markdown 或空白错误。

### Task 8: 端到端验证与变更审计

**Files:**
- Verify: `include/iobject/IRuntimeObject.hpp`
- Verify: `src/RuntimeObject.cpp`
- Verify: `example/*.cpp`
- Verify: `IObject_规则书与设计评估.md`
- Verify: `IObject_建议保留能力树.md`
- Verify: `IObject_后续关键能力清单.md`

- [ ] **Step 1: 重新从项目根目录配置并构建静态库与示例。**

Run:

```bash
cmake -S . -B build
cmake --build build --config Debug
```

Expected: CMake 配置成功，`IObject` 静态库与 `test_01_wrapping.exe` 到 `test_08_subscribe_channel.exe` 全部生成。若仅出现已知环境的 `pwsh.exe` 提示但命令退出码为 0，记录该环境提示而不将其归因于本次 API 改动。

- [ ] **Step 2: 运行所有示例，核对新的 API 语义。**

Run:

```bash
./build/example/Debug/test_01_wrapping.exe
./build/example/Debug/test_02_topology.exe
./build/example/Debug/test_03_event.exe
./build/example/Debug/test_04_type_conversion.exe
./build/example/Debug/test_05_data_channel.exe
./build/example/Debug/test_06_data_channel_change.exe
./build/example/Debug/test_07_pointer.exe
./build/example/Debug/test_08_subscribe_channel.exe
```

Expected: 所有程序以 0 退出；其中：

- `test_03_event.exe` 的事件发布和载荷生命周期演示使用 `PublishEvent`；
- `test_06_data_channel_change.exe` 表明业务可先写数据、后调用 `PublishChannel` 通知；
- `test_07_pointer.exe` 表明绑定透明指针将通道通知交给绑定目标，未绑定指针静默；
- `test_08_subscribe_channel.exe` 表明成员和自由 `PublishChannel` 都可触发同步，且非标准低层事件不触发同步。

- [ ] **Step 3: 对生产代码、示例和规则书执行旧 API 清零扫描。**

Run:

```bash
grep -R -n -E "\bPublish\s*\(" include src example IObject_规则书与设计评估.md IObject_建议保留能力树.md IObject_后续关键能力清单.md
grep -R -n -E "\bPublishEvent\s*\(|\bPublishChannel\s*\(" include src example
grep -R -n "PublishChannelChanged" example include src
git diff --check
git diff --stat
```

Expected: 第一条和第三条均无输出；第二条显示声明、实现、同步内部调用及示例调用；`git diff --check` 无输出；`git diff --stat` 仅包含本计划列出的源码、示例和三份规则文档。不要扫描或修改 `docs/superpowers/specs/` 与历史 `docs/superpowers/plans/` 中为说明历史重命名而保留的旧 API 文本。

- [ ] **Step 4: 人工复核关键不变量，不执行 Git 提交。**

逐项确认以下事实：

```text
1. 公开接口只保留 PublishEvent，不存在旧 Publish 兼容别名。
2. PublishChannel 仅发送标准 DataChannelChanged，不读取或写入通道。
3. WriteData 没有自动调用 PublishChannel。
4. 成员与自由 PublishChannel 都存在；自由函数只做空指针判断并委托成员。
5. 普通对象对空通道或已 Release 状态静默；透明指针对空绑定静默。
6. 透明指针在 PublishEvent(..., true) 未绑定时仍销毁已转移所有权的载荷。
7. Channel synchronization 在目标 WriteData 成功后复用目标 PublishChannel，以支持下游传播。
8. PublishChannel 创建的标准载荷由框架自动接管，示例和规则书不要求用户手动析构。
```

Expected: 八项都可在最终源代码、示例输出与规则书中对应验证；工作区保持未提交，等待用户之后明确决定是否进行 Git 操作。
