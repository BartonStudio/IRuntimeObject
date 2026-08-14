# IRuntimeObject 规则书与设计评估

> 本文描述当前静态库公开边界，以及暂停功能恢复前的最小对象内核。

## 1. 公开边界

`IObject` 是 C++20 静态库。安装后有以下三个公共头：

- `<iobject/IRuntimeObject.hpp>`：对象拓扑、结构事件、`As<T>()` 查询与只读数据通道契约；
- `<iobject/IRuntimeObjectPointer.hpp>`：可换绑透明指针节点契约；
- `<iobject/Runtime.hpp>`：创建节点、承载原生对象生命周期、`TypeBuilder<T>` 类型规则及可选原生数据读取适配的门面。

`RuntimeObject` 是静态库 `src` 中的私有实现，不是公共类型，调用方不得包含或依赖它。`Runtime` 工厂以 `IRuntimeObject*` 返回节点；调用方负责对每个返回节点执行 `delete`。`Connect` 仅保存非拥有拓扑边，调用方管理节点生命周期。

`IRuntimeObject` 是运行时节点契约，不是业务类型的公开基类。外部业务类型不得继承并自行构造 `IRuntimeObject`，也不应实现其虚函数后直接接入拓扑；这会绕过框架的生命周期、拓扑和事件登记规则。业务类型应保持普通 C++ 类，通过 `Runtime` 工厂包装。该约定写在公开头文件的接口注释中；框架内部实现保留构造和接入节点的责任。

## 2. 基本术语

| 术语 | 含义 |
| --- | --- |
| 原生对象 | 业务定义的普通 C++ 对象，例如 `Device`、`Hero`。 |
| `IRuntimeObject` | 稳定的非模板运行时节点契约。 |
| `Runtime` | 仅含静态方法的公开创建门面。 |
| 纯运行时节点 | 由 `Runtime::make()` 创建、不承载原生对象的节点。 |

## 3. Runtime 创建与承载规则

所有节点由 `Runtime` 创建，并以 `IRuntimeObject*` 返回：

- `Runtime::make()`：创建纯运行时节点，可作为对象拓扑的根节点；
- `Runtime::make<T>(args...)`：创建并持有新建 `T`；
- `Runtime::share(std::shared_ptr<T>)`：保留共享控制块；
- `Runtime::ref(T&)`：仅借用对象，调用方负责原对象生命周期；
- `Runtime::fromPtr(T*, owned)`：适配遗留裸指针；`owned` 为真时节点被 `delete` 时会 `delete` 指针；
- `Runtime::makePointer()`：创建未绑定的 `IRuntimeObjectPointer`；
- `Runtime::makePointer(IRuntimeObject*)`：创建后立即尝试绑定目标；目标无效时仍返回未绑定指针节点。

普通节点的 `Release()` 解除节点的全部拓扑关系及其相关事件、数据通道订阅关系，不释放运行时节点或承载的原生对象。节点被调用方 `delete` 时，承载的原生对象才按相应持有策略处理。当前尚未提供 `DeleteLater` 或统一事件循环；未来若引入单线程事件循环，将由延迟删除机制处理回调栈中的销毁请求。

### 3.0 运行时调用栈与物理删除时机

当前框架的事件、拓扑操作和数据通道均为同步调用，并允许在回调中重入其他运行时操作。一次最外层 `Publish`、`Connect`、`Disconnect`、`ReadData` 或 `WriteData`，连同其同步触发的事件、数据回调、`SubscribeChannel` 传播及嵌套调用，共同构成一个运行时调用范围。

运行时调用范围尚未返回时，不得直接物理 `delete` 可能仍被本次调用访问的 `IRuntimeObject` 节点。特别是事件处理器、`ReadData` 接收器、原生对象的 `WriteData` 实现、拓扑事件回调和同步传播过程中，都不应删除当前或相关拓扑/同步组成员；否则快照中的非拥有指针可能立即变成悬垂指针，导致未定义行为或同步只完成部分成员。

安全删除至少需要同时满足：

1. 当前最外层运行时调用及其全部同步嵌套回调已经返回；
2. 节点已经不再需要参与拓扑、订阅、数据同步或其他业务访问；
3. 调用方确认自己拥有该节点的唯一物理删除责任，且没有其他代码仍持有并访问它。

在当前模型中，业务方如需在回调栈中释放节点，应先记录待处理节点，等最外层运行时调用返回后，再执行 `Release()` 和 `delete`。未来设计单线程事件循环时，应增加 `DeleteLater` 一类的延迟删除机制：它只提交销毁请求，由事件循环在当前任务及其嵌套调用结束后执行 `Release()` 与物理 `delete`。该机制属于未来能力，当前接口不提供。

`Release()` 与 `delete` 始终是两个不同动作：`Release()` 负责退出 IRuntimeObject 系统，`delete` 负责销毁 C++ 节点及其按持有策略管理的原生对象。未来延迟删除所需的“待处理”状态是运行时内部调度细节，不作为当前公开 `Release()` 状态的一部分；当前不新增任何 `Queued` 枚举或相关接口。

### 3.1 透明指针节点

`IRuntimeObjectPointer` 是不拥有目标生命周期的可换绑节点。它通过 `Bind(IRuntimeObject*)` 绑定普通活动运行时节点，`Unbind()` 解除绑定，`GetBindObject()` 读取当前目标，`IsBound()` 查询状态；`Bind(nullptr)` 等同于 `Unbind()` 并成功。第一版拒绝绑定自身、另一指针节点、已 Release 的节点及不同运行时拓扑中的节点。

已绑定指针在**调用当次**透明转发 `SubscribeEvent`、`Publish`、数据通道和拓扑查询/操作到目标；未绑定时返回对应失败或空结果，不缓存操作。指针不维护自己的处理器、订阅或拓扑边，换绑不会迁移或撤销先前经它建立在旧目标上的关系，调用方须自行管理 `RuntimeSubscription` 句柄。`Publish` 的 `data` 始终是事件载荷而非目标参数，不会被解引用。

`pointer->As<IRuntimeObjectPointer>()` 返回指针节点自身；其余 `pointer->As<T>()` 在已绑定时查询当前目标。普通节点将 pointer 用作 `Connect` 的 child 或 `SubscribeEvent` 的 source 参数时，也只在该次调用解引用当前目标：空指针节点失败，既有边与订阅不会随之后的 `Bind()` 改变。

目标 `Release()` 或析构时，运行时会自动解除全部指向它的指针绑定，避免悬垂指针；指针从不 `Release` 或 `delete` 目标。指针自身 `Release()` 只撤销绑定并停用该指针，不影响目标，也不产生 `Released` 事件；调用方仍须 `delete` 指针节点。

### 3.2 类型转换规则

`IRuntimeObject::As<T>()` 是唯一的原生对象类型查询入口：成功时返回非拥有的 `T*`，失败时返回 `nullptr`。`const IRuntimeObject::As<T>()` 返回 `const T*`，不会允许通过结果修改包装对象。纯运行时节点、未登记目标类型的节点与已 `Release()` 的节点均返回 `nullptr`。

每一种被包装的原生类型默认支持其自身：`Runtime::make<Hero>()` 创建的节点可直接 `As<Hero>()`。业务类可在类内部声明可选静态函数，集中登记额外对外暴露的类型：

```cpp
class Hero : public ICharacter {
public:
    static void RegisterTypes(iobject::TypeBuilder<Hero>& types) {
        types.As<ICharacter>();
        types.As<IWeapon>(&Hero::weapon);
        types.As<IEquipment>([](const Hero& hero) -> IEquipment* {
            return hero.equipment;
        });
    }
};
```

`RegisterTypes` 不存在也完全合法；框架只保留默认的原类型转换。类型规则在该源类型首次被包装时初始化一次。框架对“需要尝试转换时使用 `As<T>()`”采用统一调用约定：`IRuntimeObject::As<T>()` 用于被包装原生对象的公开类型投影；`RuntimeObjectEvent::data` 同样是事件载荷节点的只读 `IRuntimeObject` 视图，处理器通过 `event.data->As<T>()` 取得其包装的普通 C++ 类型。两者成功时返回非拥有指针，失败时均返回 `nullptr`：

- `types.As<Target>()` 登记原对象直接可转为 `Target*` 的关系，通常用于公开继承；
- `types.As<Target>(&Source::member)` 登记经由稳定成员地址取得的关系；成员地址在对象存活期间必须稳定，不应用于容器元素等可失效地址；
- `types.As<Target>(callable)` 登记自定义业务转换。回调同时接受 `Source&` 与 `const Source&`，并分别返回 `Target*` 与 `const Target*` 可兼容的值；可按对象状态返回 `nullptr`。

三种写法对调用方都是同一个 `As<Target>()` 语义，内部如何取得目标对象不影响查询端。每个源类型到同一目标类型最多登记一条规则；重复登记会抛出 `std::logic_error`，不会覆盖或依赖登记顺序。

该系统不是 schema、字符串类型系统、C++ 反射、动态 `invoke` 或转换链：框架不会自动枚举全部父类、成员或 `operator T*()`，也不会跨拓扑节点查找。需要公开的基类、组合成员或复杂转换必须在 `RegisterTypes` 中明确登记；用户定义的转换运算符可在自定义回调内自行调用。`As<T>()` 返回的是非拥有指针，不改变原生对象的生命周期，也不提供对象所有权提取。

### 3.2 数据通道

`IRuntimeObject::ReadData(DataChannelView channel, DataReceiver receiver) const` 提供同步的一次性不透明字节读取，`IRuntimeObject::WriteData(DataChannelView channel, ByteInput data)` 提供同步写入。通道是大小写敏感的业务字符串，例如 `"State"` 或 `"NetworkSnapshot"`；框架不维护全局通道表，也不会将通道解释为属性名、schema 或序列化格式。

原生类可选择实现同名成员函数，从而在被 `Runtime` 包装时自动接入：

```cpp
bool ReadData(iobject::DataChannelView channel,
              iobject::DataReceiver receiver) const;

bool WriteData(iobject::DataChannelView channel,
               iobject::ByteInput data);
```

两项能力相互独立：只实现读取、只实现写入或均不实现都不影响包装、拓扑、事件和 `As<T>()`。纯运行时节点、已 `Release()` 的节点、空通道、未实现对应能力、未知通道或原生类主动拒绝操作时，接口返回 `false`。`ReadData` 返回 `true` 表示同步调用接收器恰好一次，传入空 `ByteView` 同样是有效的成功结果；`WriteData` 返回 `true` 只表示原生对象已同步接受并处理输入，不承诺数据必然发生变化。

`ByteView` 只在读取接收器调用期间有效；需要长期保留时，调用方必须在回调中复制字节，不能保存 `data()` 指针或视图。`ByteInput` 也只在 `WriteData` 当前调用期间有效；原生类若要长期保存输入必须自行复制。原生类自行决定数据组成与编码，框架不解释其内容。不要通过 `reinterpret_cast` 直接把含指针、`std::string`、容器或虚表的 C++ 对象内存暴露为跨边界数据。

数据读写与变化通知是刻意分离的两个业务动作。框架不会因业务直接调用 `WriteData` 返回成功而自动发布事件，也不会验证数据是否真实变化；原生对象在自身运行中直接修改成员变量时同样不会被框架自动感知。业务方确认某通道发生需要观察者处理的逻辑变化后，应显式发布：

```cpp
object->Publish(
    iobject::RuntimeEventTypes::DataChannelChanged,
    iobject::Runtime::make<iobject::DataChannelChangedEventData>("State"),
    true);
```

### 3.3 数据通道订阅

`SubscribeChannel` 是已实现的本地、同步、无初始快照的通道同步关系。它有三种等价入口，均返回可移动、不可复制的 `RuntimeSubscription`：

```cpp
// 当前对象是 target，源与目标通道同名。
target->SubscribeChannel(source, channel);

// 当前对象是 target，允许将源通道映射到另一目标通道。
target->SubscribeChannel(source, sourceChannel, targetChannel);

// 不以某一端作为调用者的公共形式。
iobject::SubscribeChannel(source, sourceChannel, target, targetChannel);
```

建立订阅只登记关系，绝不读取、写入或执行初始同步。源、目标、任一通道为空，端点已 `Release()`、析构、失效或不在同一运行时拓扑时，返回失效句柄。`IRuntimeObjectPointer` 作为 source 或 target 时仅在建立调用的当次解引用；之后 `Bind()`、`Unbind()` 或换绑均不会迁移已建立关系。

通道同步只识别规范的变化通知：事件类型必须精确为 `RuntimeEventTypes::DataChannelChanged`，`event.data` 必须非空且 `event.data->As<DataChannelChangedEventData>()` 成功，并且事件源和载荷中的 `channel` 必须分别精确匹配登记的源对象与源通道。其他事件、空或错误载荷、以及通道不匹配均被静默忽略，不影响普通事件订阅。

每一条匹配关系在当前调用栈内依次执行“源 `ReadData` → 接收器恰好一次 → 目标 `WriteData`”。读取失败、接收器不是恰好一次、写入失败、端点在过程中 `Release` / 析构 / 失效，都会静默停止该分支。只有这三步都成功后，框架才会自动向目标发布带目标通道的规范 `DataChannelChanged`，使下游订阅继续同步；这项自动发布仅属于 `SubscribeChannel` 的成功同步过程，业务直接调用 `WriteData` 后仍须自行 `Publish` 变化事件。

单次通道变化传播维护独立的当前路径。某分支准备进入的目标已在其路径中时，框架会向 `std::cerr` 输出：

```text
[IObject] 数据通道同步截断：检测到重复节点
```

随后停止该分支。例如 `A -> B -> C -> B -> E` 在第二次进入 B 时终止，E 不会执行；兄弟分支彼此独立。事件派发的全局保险同样适用于其自动发布：最大嵌套深度为 32、单链路最大成功发布次数为 128。

`RuntimeSubscription` 的析构和幂等 `Cancel()` 均会解除通道订阅；源或目标 `Release` / 析构时也会自动解除，失效句柄的 `IsActive()` 返回 `false`。`SubscribeEvent` 与 `SubscribeChannel` 共用这一句柄模型。普通事件订阅者仍可在收到 `DataChannelChanged` 后自行检查通道并按需 `ReadData`；框架不直接向它们推送字节，不缓存数据，也没有版本号、错误对象或后台队列。原生读写逻辑、读取接收器和事件处理器抛出的 C++ 异常保持原样向调用方传播，异常策略由业务代码负责。

### 3.4 数据通道后续演进（未来设计）

当前本地同步保持同步、逐次、无状态：一次变化会立即读取源通道并写入目标，不保存字节、不压缩连续变化，也不向后台提交任务。以下能力尚未实现；`SubscribeChannel` 只是未来远程能力可复用的本地基础：

- **通道缓存**：为对象与通道保存最近一次成功读取的字节副本。它可供新订阅关系取得初始快照、在短暂不可读或远端断线时使用最后有效值，并让一次读取服务多个目标；同时必须定义内存上限、缓存失效与对象 `Release` 后的清理规则。
- **变更合并**：将同一对象、同一通道在一个未来事件循环批次内的多次连续变化压缩为最终状态，降低高频状态同步的读取、复制和写入开销。合并会丢失中间状态，因此只适用于状态快照，不能默认用于每次操作都必须可见的命令、日志或交易类通道。
- **异步任务队列**：把通道读取、写入和传播从当前发布调用栈延后到未来事件循环执行。它是控制长传播链、支持合并、优先级、跨线程与 `deleteLater()` 安全析构的基础；引入后必须明确执行顺序、可见时机、任务取消、失败与源/目标在任务执行前 `Release` 的行为。
- **远程传输与桥接**：远程代理、连接桥接、稳定对象身份映射和网络协议均未实现。未来若接入 Socket、WebSocket、共享内存或串口等传输，必须另行定义连接、初始快照、重连、顺序、重复投递、双向回环与权限语义，不能将其视为当前本地订阅的既有能力。

## 4. 拓扑规则与事件

```cpp
iobject::IRuntimeObject* root = iobject::Runtime::make();
iobject::IRuntimeObject* child = iobject::Runtime::make();
root->Connect("health", child);

// 在不再使用节点时，调用方负责释放。
delete child;
delete root;
```

- 拓扑是允许多父的 DAG；同级子名称唯一，`Connect` 以 `IRuntimeObject*` 建立双向索引的非拥有边；连接名称必须非空且不含 `.`，`child == nullptr`、自环或可形成间接环时返回 `false`；
- 父节点绝不 `delete` 子节点，调用方保有并管理节点；节点 `Release` 或析构时会自动解除全部相关拓扑边；
- 会形成环的 `Connect` 是原子失败：不改变现有边，也不发送事件；
- 默认 `overwrite == false`：重名连接返回 `false`，不改变关联且不发送事件；
- `overwrite == true`：通过环检测后先完成拓扑替换，再同步依次发送同名的 `ChildDisconnected`、`ChildConnected`；覆盖不销毁旧子节点；
- 新名称连接成功后，先记录拓扑边，再同步发送 `ChildConnected`；
- `Disconnect` 只移除当前父节点的直接子边，且只接受非空、不含 `.` 的单层名称；`GetChildItem` 接受 `.` 分隔的相对向下路径并逐层查询，返回非拥有的 `IRuntimeObject*`；空路径、首尾点、连续点或任一层不存在时为 `nullptr`。当前不支持绝对路径、`.`、`..`、通配符或跨父查询，因为多父 DAG 不存在唯一父路径；
- `Release()` 第一次调用会先解除所有入边与出边，再为每条入边由原父节点、为每条出边由当前节点同步发送 `ChildDisconnected`；随后在清理订阅前发送一次 `Released`，最后清理全部订阅与本地处理器。重复调用安全。释放后不能再 `Connect`、注册处理器、观察或发布事件，查询结果为空；
- `delete` 未预先 `Release()` 的节点时，析构会执行同一释放流程并发送一次 `Released`；先 `Release()` 再 `delete` 不会重复发送；
- `GetChildren() const` 是只读查询，返回名称和非拥有 `IRuntimeObject*` 组成的快照，不暴露内部容器；其中指针在 `Disconnect`、`Release`、覆盖、子节点析构或父节点析构后失效；
- 成功 `Disconnect` 会在拓扑更新后同步发送 `ChildDisconnected`；业务方可通过 `Publish` 发布自定义字符串事件；
- `RuntimeEventTypes` 命名空间公开内置字符串常量：`ChildConnected`、`ChildDisconnected`、`DataChannelChanged`、`Released`。它们与业务自定义字符串完全共用 `RuntimeEventTypeView` 的唯一 API 和统一的字符串键匹配规则；
- `SubscribeEvent(source, type, handler)` 是唯一的事件登记接口，接受 `RuntimeEventTypes` 中的内置常量或自定义字符串。调用者是订阅者，source 是事件源，三者必须在同一运行时拓扑内且处于活动状态；监听自身也必须显式传入自身。字符串大小写敏感，空 source、type 或 handler 不会建立订阅，空字符串发布无操作；事件类型在内部复制保存，调用方传入的临时字符串可安全使用；
- `RuntimeObjectEvent::data` 是可空、非拥有的 `const IRuntimeObject*` 事件载荷视图，仅在当前同步回调期间有效，不得保存。需要取得具体事件数据时统一调用 `event.data->As<T>()`，不直接使用 `dynamic_cast`；该转换与普通运行时节点一致，支持原包装类型及其 `RegisterTypes` 规则。`ChildConnected` 与 `ChildDisconnected` 使用包装后的 `ChildEventData`，其中包含子边名称和非拥有 child 指针；`DataChannelChanged` 使用包装后的 `DataChannelChangedEventData`，其中包含发生逻辑变化的通道名称。业务载荷可通过 `Runtime::make<T>(...)` 构造。该事件只通知变化，观察者收到后应按需调用事件源的 `ReadData` 读取当前数据；
- `Publish(type, data, false)` 只借用非空 `data`，调用方仍负责其 `delete`；`Publish(type, data, true)` 从调用开始接管独占删除责任，在完整同步派发结束、未投递或处理器异常离开时均会 `delete data`。不得将同一指针重复以 `true` 交付，也不得将当前 `event.data` 以 `true` 交给嵌套 `Publish`；嵌套发布各自管理载荷，内层结束先析构内层载荷，不影响外层载荷；
- `Released` 表示节点已退出 IRuntimeObject 对象系统，实际发生在节点内存失效前。其 `source` 只可在当前回调中作地址身份比较，不应调用或保存；
- 其他事件的 `RuntimeObjectEvent::source` 也只应在处理器执行期间使用；
- 订阅关系直接保存 `EventHandler`，不再拆分本地处理器表与对象间观察关系；`SubscribeEvent(source, type, handler)` 原子建立一条订阅并返回可移动且不可复制的 `RuntimeSubscription`。句柄析构自动取消，也可调用幂等的 `Cancel()`；任一端 `Release` 或 `delete` 都会自动解绑，失效句柄的 `IsActive()` 返回 `false`；
- 发布按事件源和类型查找订阅快照，并同步执行各订阅保存的回调；没有登记回调的对象不会形成有效订阅，事件直接丢弃，不保存收件箱或队列；
- 处理器按 ID 顺序快照执行，处理器中增删处理器或取消订阅不破坏当前派发；普通 `Publish`、`Connect`、`Disconnect` 不捕获处理器异常，处理器不得向框架抛出 C++ 异常；`Release` / 析构路径仅为析构安全而吞掉异常。当前仍假定单线程事件循环，不是全局事件总线，也没有异步队列；
- 同一最外层事件及其同步嵌套发布共享私有派发上下文。若当前活跃路径重复出现相同的 `(事件源指针, 事件类型)`，框架输出诊断并截断该次待发布事件；不禁止自订阅或双向订阅；
- 同一事件链最大嵌套发布深度为 32、最大成功发布次数为 128。第 33 层或第 129 次待发布事件会向 `std::cerr` 输出中文诊断并被截断；此前已完成的派发不回滚，调用正常返回。

## 5. 构建、安装与使用

构建目标是 `IObject`，项目内可使用别名 `IObject::IObject`。安装会导出 CMake 包，外部项目可使用：

```cmake
find_package(IObject CONFIG REQUIRED)
target_link_libraries(client PRIVATE IObject::IObject)
```

安装内容仅包括静态库、上述两个头文件和 CMake package 配置。内部工厂桥接位于 `iobject::detail`，仅为模板门面链接静态库所需，调用方不应直接依赖。

## 6. 当前拓扑实例与多域演进

当前只有一个进程内函数局部 `static RuntimeTopology`，所有 `Runtime` 工厂创建的节点均接入该单一全局拓扑。节点创建时会缓存一个非拥有的 `RuntimeTopology* topology_`，后续 `Connect`、`Disconnect`、查询与释放均直接通过该指针访问拓扑；该缓存不改变当前公开 Runtime API、DAG 规则、事件或生命周期。

未来可由全局 `RuntimeDomainManager` 管理多个 `RuntimeDomain`，每个域独立持有一个 `RuntimeTopology`。届时连接仅允许发生在同一域；跨域 `Connect` 的第一版应返回 `false`，跨域引用留待后续模型解决。域的生命周期必须晚于其中所有节点的析构，因此节点保存的非拥有 `topology_` 指针在节点存活期间始终有效。这是规划，当前尚未实现多域管理或跨域规则。

缓存 `topology_` 只消除了每次操作重新取得全局单例的间接访问，不能解决 `RuntimeTopology::rebuildIncoming()` 在每次拓扑变更时全图重建入边索引的性能问题；该问题需要独立的数据结构优化。

## 7. 当前边界与后续规划

当前内核仅包含：**`IRuntimeObject` 契约 + 私有静态库实现 + `Runtime` 创建门面 + 显式 `As<T>()` 类型转换 + 只读不透明数据通道 + 命名非拥有拓扑 + 对象级结构事件**。

恢复动态访问、方法、属性之前，应先定义统一 `Error` / `Result`、稳定对象 ID 与路径、批量变更、来源上下文和线程模型。示例 `example/01_WrappingTest.cpp` 仅演示创建方式、承载策略与调用方 `delete` 节点；`example/04_TypeConversionTest.cpp` 演示 `RegisterTypes` 与 `As<T>()`；`example/05_DataChannelTest.cpp` 演示可选原生 `ReadData`、空数据和失败路径。它们均不是自动化单元测试。
