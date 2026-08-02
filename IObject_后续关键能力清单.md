# IRuntimeObject 后续关键能力清单

> 本文规划 IRuntimeObject 从当前最小对象拓扑与结构事件内核演进为同步、规则、场景、命令与响应式业务的基础设施；下列能力均未在当前公开接口中实现。

## 当前内核边界

```text
IRuntimeObject
├─ IRuntimeObjectPointer（透明、非拥有、可换绑）
├─ 对象级事件
│  ├─ AddEventHandler / RemoveEventHandler
│  ├─ Observe / RuntimeSubscription
│  └─ 内置字符串常量与自定义字符串事件
└─ 对象拓扑
   ├─ Connect / Disconnect
   ├─ Release（仅解除拓扑）
   ├─ child lookup
   └─ child enumeration（GetChildren() const 只读快照）

Runtime
├─ value / shared_ptr / reference / legacy raw pointer 生命周期承载
└─ makePointer([IRuntimeObject*])
```

公开头为 `<iobject/IRuntimeObject.hpp>`、`<iobject/IRuntimeObjectPointer.hpp>` 与 `<iobject/Runtime.hpp>`；具体节点实现是私有静态库代码。`Runtime` 工厂返回的节点均由调用方 `delete`。`IRuntimeObjectPointer` 是已实现的非拥有透明指针节点：它可绑定同一运行时拓扑内的普通活动节点，`Bind(nullptr)` 等价解绑；它转发当前调用到目标，但不保存处理器、订阅或拓扑关系，换绑不会迁移历史关系。普通节点以它作为 `Connect` child 或 `Observe` source 时只对当次调用解引用；空 pointer 失败。目标 Release 或析构会自动让全部相关 pointer 解绑；pointer Release 只停用 pointer 自身，不影响目标且不发布 `Released`。`Connect` 仅保存双向索引的非拥有边，当前拓扑为允许多父的 DAG；连接名称必须是非空、不含 `.` 的单层名称，自环或间接环连接原子返回 `false`，保留旧边且不发送事件。`GetChildItem` 是从当前节点向下逐段解析的点分相对路径即时查询，不是稳定身份、绝对路径协议或跨域解析；多父 DAG 下不支持 `..`。普通 `Release()` 只解除相关拓扑，不释放节点或原生对象；元对象仅在节点被调用方 `delete` 时按对应策略处理。当前没有对象访问、动态调用、方法注册、属性或有效性 API。

## 多运行时域与跨域引用（未实现）

当前所有节点共享单一全局 `RuntimeTopology`，没有域概念。未来需要引入以下能力，但不改变当前最小内核：

- `RuntimeDomainManager`：全局管理多个 `RuntimeDomain`，每个域拥有独立的 `RuntimeTopology`，并负责保证域晚于其中节点析构；
- `DomainId`：稳定标识运行时域，以便 API、事件、诊断与持久化内容区分节点所属域；
- 域内稳定对象 ID 索引：按域维护 `RuntimeObjectId -> IRuntimeObject*`（或受控句柄）的索引，支撑可靠解析、事件与后续同步；
- `ExternalObjectRef`：表达跨域引用的显式模型，而不是把跨域对象直接加入当前域的 DAG。

第一版多域实现中，`Connect` 仅允许同域节点；跨域连接应返回 `false`。上述管理器、身份索引和 `ExternalObjectRef` 均为未来规划，当前未实现。

## 当前事件系统的边界与后续议题

每个节点组合一个私有 `RuntimeEventDispatcher`，由它在节点本地保存 `EventHandlerId -> {事件类型, EventHandler}` 处理器表；它不是全局事件总线。`RuntimeEventTypes` 命名空间公开 `ChildConnected`、`ChildDisconnected`、`DataChannelChanged`、`Released` 四个内置字符串常量；它们与业务自定义字符串共用同一个 `RuntimeEventTypeView` API，并按大小写敏感的字符串键匹配。事件可携带非拥有的 `IRuntimeObject*` 载荷；结构事件使用包装后的 `ChildEventData`，业务载荷可用 `Runtime::make<T>(...)` 创建，并在处理器中以 `event.data->As<T>()` 读取。`AddEventHandler` / `RemoveEventHandler` 只注册或删除当前节点处理器；`Connect`、`Disconnect`、`Release` 与 `Publish` 在事件源本地先同步派发，再按对象间订阅投递。空字符串不能注册或订阅，发布空字符串无操作。派发器按 ID 顺序复制匹配类型的处理器快照，处理器内的增删只影响下一次派发。普通发布路径不捕获异常，处理器不得向框架传播 C++ 异常；仅 `Release` / 析构路径为析构安全而吞掉异常。当前所有流程假定在同一线程、同一事件循环中完成。

### 对象间订阅术语与当前语义

对象间订阅仅允许 `IRuntimeObject` 参与：订阅者与事件源必须都是同一运行时拓扑中的活跃 `IRuntimeObject`；普通 C++ 规则对象、同步器、UI 等若需监听，必须先包装为 `IRuntimeObject` 节点。

| 术语 | 含义 |
| --- | --- |
| 事件源（Source） | 发布事件的 `IRuntimeObject`，例如 B 发布 `ChildConnected` 或业务自定义事件。 |
| 订阅者（Subscriber / Observer） | 订阅事件源的 `IRuntimeObject`，例如 A 订阅 B。 |
| 订阅关系（Subscription） | A 与 B 之间关于某类事件的双向可取消关系。 |
| 投递（Delivery） | B 发布事件后，框架按订阅关系将事件同步发送给 A。 |
| 处理器（Handler） | A 为收到的事件可选注册的处理回调。订阅与处理器注册是两件独立的事。 |

当前语义：A 以 `Observe(B, type)` 订阅 B 的某种事件后，B 发布该事件时框架会尝试投递给 A；`type` 可以是 `RuntimeEventTypes` 中的内置字符串常量，也可以是自定义字符串。若 A 没有匹配处理器，事件直接丢弃，不保存收件箱、不排队、不支持事后读取。订阅关系与处理器注册相互独立，可存在没有处理器的有效订阅。`RuntimeObjectEvent::data` 是仅在当前同步回调期间有效的、非拥有的 `const IRuntimeObject*` 视图；它可为空，业务载荷通过 `Runtime::make<T>(...)` 包装，并以 `event.data->As<T>()` 读取。`Publish(type, data, false)` 只借用载荷，调用方仍负责 `delete`；`Publish(type, data, true)` 在完整同步派发结束或异常离开时自动 `delete data`，调用方之后不得再使用该指针。回调作者必须自行处理业务错误，回调不得向运行时框架传播 C++ 异常。

`Released` 在拓扑已解除、订阅清理前同步投递；它表示节点已退出 IRuntimeObject 对象系统，实际发生在节点内存失效前。`source` 仅可在此回调中作地址身份比较，不应调用或保存。显式 `Release()` 或未预先 Release 的 `delete` 各最多发送一次；若已 Release，后续 delete 不重复投递。

每次最外层同步发布都会创建私有链路上下文，回调中的嵌套发布复用它。当前活跃路径重复出现同一 `(事件源指针, 事件类型)` 时会诊断并截断该待发布事件；这不禁止自订阅或双向订阅。链路最大嵌套深度为 32、最大成功发布次数为 128；超出时向 `std::cerr` 输出诊断并截断，不回滚先前已完成的派发。

`RuntimeSubscription` 由拓扑维护订阅者和事件源的双向索引：在 `Released` 投递后，A 或 B `Release` / `delete` 都会自动取消相关订阅；句柄支持手动、幂等的 `Cancel()` 与 RAII 析构取消，解除后的 `IsActive()` 返回 `false`。当前仅支持按字符串事件键的完全匹配过滤；异步队列、条件过滤和持久化收件箱仍未实现。

以下能力尚未实现，仅作为后续设计议题记录：

- 异步队列、延迟投递、跨线程调度与执行器绑定；
- 按事件类型、路径或条件筛选订阅；
- 父子层级的自动传播、冒泡与捕获阶段；
- 事件取消、阻止传播与事件优先级；
- 更丰富、可版本化的事件负载与结构化变更记录；
- 批量操作的事件合并、事务提交边界与回滚通知；
- 变更来源、关联 ID、同步回环抑制、审计上下文与权限上下文；
- 面向远程同步、序列化和重放的事件顺序、幂等性与版本协议。

这些能力均不应在未定义线程、错误、生命周期和身份语义前直接加入当前最小事件内核。

# 第一梯队：同步与响应的基础

## 1. 稳定对象身份与寻址

需要同时定义不可变的 `RuntimeObjectId` 与人类可读的 `RuntimeObjectPath`，以供事件、命令、同步包、场景引用和规则长期定位节点。名称对象拓扑不能替代稳定 ID。

建议最小接口：

```cpp
RuntimeObjectId GetId() const;
std::string GetPath() const;
IRuntimeObject* ResolvePath(std::string_view path);
```

## 2. 统一错误与结果模型

为未来的对象访问、方法、属性与路径解析定义一致的 `Error` / `Result`，明确失败原因和目标位置。当前 `Connect` / `Disconnect` 的 `bool` 仅覆盖最小结构操作。

## 3. 属性、方法与可观察状态

在错误与生命周期规则明确后，再设计对象承载访问、动态方法和属性元数据。属性变更事件应与属性系统一起引入；不得从旧的公开 `RuntimeObject` 设计恢复接口。

# 第二梯队：安全传播变化

## 4. 批量变更与事务边界

聚合一组结构或状态修改，使订阅者在提交后看到完整状态。初版可只定义提交边界与事件聚合，跨对象回滚由后续命令系统负责。

## 5. 变更来源与调用上下文

定义 `ChangeOrigin`、调用者和关联 ID，以支持同步回环抑制、审计和规则过滤。

## 6. 命令模型

将 `ConnectChildCommand`、`DisconnectChildCommand` 及未来的状态修改封装为可验证、可执行和可记录的业务意图，而不是将命令历史塞进 `IRuntimeObject`。

# 第三梯队：扩展运行时生态

1. 属性元数据与结构化序列化；
2. 线程与执行器模型；
3. 场景、同步协议、规则、状态机、调度、通知与统计。

# 推荐实施顺序

```text
稳定身份与路径
  -> Error / Result
  -> 对象访问、方法与属性语义
  -> 批量变更与 ChangeContext
  -> 命令、权限、审计、撤销重做
  -> 元数据、并发、序列化与同步生态
```
