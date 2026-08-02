# IRuntimeObject 建议保留能力树

```text
IRuntimeObject（稳定的非模板运行时契约）
├─ IRuntimeObjectPointer（透明、非拥有、可换绑指针节点）
│  ├─ Bind / Unbind / GetBindObject / IsBound
│  ├─ As<IRuntimeObjectPointer> 识别自身，其余 As<T> 转发目标
│  └─ 目标 Release 或析构时自动解绑
├─ 对象级事件
│  ├─ AddEventHandler / RemoveEventHandler（节点本地处理器）
│  ├─ Observe / RuntimeSubscription（对象间关系）
│  ├─ 内置字符串常量 + 自定义字符串事件
│  └─ IRuntimeObject* / ChildEventData（结构化事件载荷）
└─ 对象拓扑
   ├─ Connect / Disconnect（IRuntimeObject* 非拥有边）
   ├─ Release（只解除全部入边与出边）
   ├─ child lookup（GetChildItem：点分相对路径、非拥有视图）
   └─ child enumeration（GetChildren() const 只读快照）

Runtime（公开创建门面，返回 IRuntimeObject*）
├─ make()
├─ make<T>()
├─ share(shared_ptr<T>)
├─ ref(T&)
├─ fromPtr(T*, owned)
└─ makePointer([IRuntimeObject*])

src/RuntimeObject.cpp（私有静态库实现）
├─ 节点生命周期承载
├─ 按事件类型的本地处理器表
├─ 多父 DAG 与对象间订阅的双向非拥有索引
└─ 普通目标到透明指针节点的反向非拥有绑定索引
```

## 使用边界

- 上层模块（规则、同步、工具、命令）应依赖 `IRuntimeObject` 与 `Runtime`，不得依赖私有 `RuntimeObject` 实现。
- 业务接入代码包含 `<iobject/Runtime.hpp>`，使用 `Runtime` 将 C++ 对象按值、共享、借用或裸指针语义接入；工厂返回 `IRuntimeObject*`，调用方负责 `delete` 每个节点。
- 普通节点 `Release()` 解除拓扑并发送一次 `Released`，重复调用安全；它不释放运行时节点，也不释放承载的原生对象。`IRuntimeObjectPointer::Release()` 是例外：仅解绑并停用自身，不影响目标且不发布事件。
- `make<T>`、`share`、`ref` 与 `fromPtr` 创建方式承载的原生对象，仅在节点被调用方 `delete` 时按其策略处理。
- `IRuntimeObjectPointer` 非拥有地绑定普通活动节点。空指针的转发操作失败且不保留意图；`Connect` child 与 `Observe` source 传入 pointer 时只在当前调用解引用，既有边或订阅不随换绑迁移。目标 Release 或析构会自动解除相关 pointer，pointer 绝不释放目标。
- 当前没有 `isValid`、对象提取、方法调用、方法注册或属性 API；这些能力恢复前不能成为调用方契约。
- `Connect` 以 `IRuntimeObject*` 建立多父 DAG 的非拥有边，名称必须非空且不含 `.`：`nullptr`、自环或间接环返回 `false`；环检测失败是原子的，保留旧边且不发送事件。父节点绝不 `delete` 子节点；节点 `Release` 或析构时自动解除关联，覆盖只替换关联且不销毁旧节点；`GetChildItem` 支持从当前节点向下的点分相对路径，返回非拥有指针，不支持绝对路径、`..` 或跨父查询。
- `GetChildren` 返回名称与非拥有指针组成的快照；快照中的指针在 `Disconnect`、`Release`、覆盖、子节点析构或父节点析构后不得继续使用。

## 事件系统边界

- `AddEventHandler` / `RemoveEventHandler` 管理节点本地处理器，`Observe` 管理订阅者和事件源之间的独立关系；二者不可互相替代；
- `RuntimeEventTypes` 命名空间提供 `ChildConnected`、`ChildDisconnected`、`DataChannelChanged`、`Released` 内置字符串常量；它们与业务自定义字符串共享唯一的事件 API 和字符串匹配链路；
- 字符串事件大小写敏感；空字符串不能注册处理器或订阅，发布空字符串无操作；`RuntimeObjectEvent::data` 是仅在当前同步回调期间有效的非拥有 `const IRuntimeObject*` 视图，使用 `event.data->As<T>()` 取得具体载荷；结构事件通过包装后的 `ChildEventData` 提供名称和非拥有 child 指针，`Publish(type, data, true)` 会在同步派发结束后自动 `delete data`；
- 订阅者和事件源都必须是同一运行时拓扑中的 `IRuntimeObject`；发布事件时，订阅者没有匹配处理器就直接丢弃事件；
- `Released` 在节点退出拓扑后、订阅清理前同步投递；其 source 仅可在当前回调中作身份比较，不应调用或保存。显式 `Release()` 与未预先 Release 的 `delete` 各最多发送一次，先 Release 再 delete 不重复通知；
- `RuntimeSubscription` 可手动 `Cancel()`，析构自动取消；`Released` 投递后，订阅者或事件源任一端 `Release` / `delete` 都会自动解绑；
- 单线程、同一事件循环内串行使用，同步派发，不经过异步队列；
- 单条同步事件链检测当前活跃路径的重复 `(事件源, 事件类型)` 并截断环；最大嵌套深度为 32、最大成功发布次数为 128，超出时向 `std::cerr` 输出诊断并截断待发布事件；
- 处理器快照允许处理器内增删处理器或取消订阅；普通发布路径不吞异常，处理器不得向框架抛 C++ 异常，`Release` / 析构仅为安全而吞掉异常；
- 无全局总线、事务/批处理、变更来源上下文、稳定对象 ID 或路径。

## 不应直接塞入 IRuntimeObject 的能力

- 类型转换与承载对象提取；
- Schema、规则引擎、状态机、调度器；
- 网络协议、权限、场景与通用序列化；
- 动态方法与属性系统（恢复前需先定义错误和生命周期语义）。
