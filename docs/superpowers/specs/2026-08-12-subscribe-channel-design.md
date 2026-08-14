# SubscribeChannel 本地数据通道同步设计

## 目标

在不改变原生业务对象数据表示和序列化方式的前提下，建立本地、同步、无缓存的数据通道同步关系。源对象发布规范的 `DataChannelChanged` 事件后，框架读取源通道字节并写入目标通道；写入成功后由框架发布目标的变化事件，供下游同步关系继续传播。

本设计只覆盖同一运行时拓扑内的同步调用。不包含初始快照、缓存、变更合并、异步队列、跨线程或远程传输。

## 公开接口

`IRuntimeObject` 增加两个成员函数，当前对象始终是同步目标：

```cpp
RuntimeSubscription SubscribeChannel(
    IRuntimeObject* source,
    DataChannelView channel);

RuntimeSubscription SubscribeChannel(
    IRuntimeObject* source,
    DataChannelView sourceChannel,
    DataChannelView targetChannel);
```

公共命名空间函数提供源与目标均显式的写法：

```cpp
RuntimeSubscription SubscribeChannel(
    IRuntimeObject* source,
    DataChannelView sourceChannel,
    IRuntimeObject* target,
    DataChannelView targetChannel);
```

三种形式都创建同一种通道订阅关系，并返回可移动、不可复制的 `RuntimeSubscription`。`Cancel()`、句柄析构、源或目标任一端 `Release()` / 析构都会解除该关系。

## 建立规则

- 成员函数的 `this` 是目标对象。
- 两参数成员函数令源通道和目标通道同名。
- 源、目标都必须为同一运行时拓扑中的活动普通运行时节点。
- 传入 `IRuntimeObjectPointer` 时，仅在建立时解引用其当时绑定的目标；空指针节点、不同拓扑节点、空通道和已释放节点不能建立关系。
- 建立订阅不会读取源通道，也不会写入或覆盖目标现有数据。

## 变化识别与同步流程

框架只把以下事件认作可同步的通道变化：

1. 事件类型是 `RuntimeEventTypes::DataChannelChanged`；
2. `event.data` 非空，并可通过 `event.data->As<DataChannelChangedEventData>()` 得到载荷；
3. 载荷的 `channel` 与订阅记录的源通道完全匹配。

不符合以上条件的事件对该通道订阅静默无效，不影响普通 `SubscribeEvent` 处理器，也不报告错误。

对每一条匹配的通道订阅，框架按以下步骤执行：

1. 从源对象同步调用 `ReadData(sourceChannel, receiver)`；
2. 读取成功且接收器恰好交付一次字节视图后，同步调用目标对象的 `WriteData(targetChannel, bytes)`；
3. `ReadData` 或 `WriteData` 失败时，停止当前同步分支，不发布目标变化；
4. 写入成功后，框架创建 `DataChannelChangedEventData(targetChannel)` 并以目标为事件源发布 `DataChannelChanged`；
5. 该事件会照常通知普通事件订阅者，并可让目标的下游通道订阅继续传播。

业务对象自己的 `WriteData` 仍不自动发布变化通知；这里的自动通知仅发生在框架成功执行一条 `SubscribeChannel` 同步写入之后。

## 传播与循环规则

每一个最外层通道变化传播链路拥有独立上下文。上下文记录当前分支已经进入过的运行时节点。

- 初始事件源在链路开始时记为已访问。
- 每次准备把数据写入目标前，框架检查目标是否已在当前分支的访问历史中。
- 若目标已经出现，框架输出诊断并立即终止该分支；不会写入目标，也不会继续传播该分支。
- 分叉时，每个下游分支复制当前访问历史，因此一个分支的终止不会阻止其他分支同步。
- 例如 `A -> B -> C -> B -> E` 在第二次准备进入 `B` 时终止该分支，`E` 不会被访问。
- 既有事件派发的最大嵌套深度 32 与单链路最大发布次数 128 继续作为最终保险。

普通事件回调能够继续重入框架；通道重复节点规则只约束通道同步分支。

## 内部结构

通道同步不能只把 `SubscribeEvent` 返回的事件订阅句柄直接暴露给调用方。运行时拓扑需要保存专用的通道订阅记录，包括：

- 源节点和源通道；
- 目标节点和目标通道；
- 唯一订阅 ID；
- 按源节点、目标节点、源通道建立的索引。

`RuntimeSubscription` 继续通过现有控制指针与 ID 取消关系。取消入口应能根据 ID 找到事件订阅或通道订阅并进行相应清理，使其保持统一的对外 RAII 语义。

## 失败、释放与异常

- `ReadData`、读取接收器或 `WriteData` 的 C++ 异常遵循当前同步调用规则，向发布调用方传播；框架不吞掉普通发布路径异常。
- 源或目标在同步关系建立后 `Release()` / 析构时，运行时自动删除相关通道订阅记录。
- 当前版本仍要求业务代码不在运行时调用范围内直接 `delete` 可能被当前传播链访问的节点。

## 验证范围

新增中文示例程序覆盖：

- 同名通道同步；
- 映射到不同目标通道；
- 公共四参数函数；
- 写入后向下游继续传播；
- 读取失败、写入失败、非规范载荷和通道不匹配均不产生同步；
- `A -> B -> C -> B` 的重复节点分支被截断；
- 取消订阅和源/目标释放后的自动解绑。
