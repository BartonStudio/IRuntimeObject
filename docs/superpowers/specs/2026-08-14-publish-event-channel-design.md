# PublishEvent 与 PublishChannel API 设计

## 1. 目标

明确事件发布、数据写入和通道变化通知之间的职责，并为高频的通道变化通知提供统一的框架层封装。

本次设计包含两项 API 调整：

1. 将 `IRuntimeObject::Publish(...)` 彻底重命名为 `PublishEvent(...)`，不保留旧名称。
2. 提供成员函数和自由函数两种 `PublishChannel(...)` 入口，用于发布标准的 `DataChannelChanged` 事件。

本次设计只调整 API 语义和调用入口，不改变现有事件派发模型、拓扑关系、订阅生命周期或数据通道存储方式。

## 2. 核心职责划分

### `WriteData`

```cpp
bool WriteData(DataChannelView channel, ByteInput data);
```

向对象的指定数据通道提交字节数据。是否接受数据、如何解析和如何处理输入，由被包装对象的实现决定。

`WriteData` 不自动发布通道变化事件。业务层在确认逻辑状态发生变化后，应显式调用 `PublishChannel`。

### `PublishChannel`

```cpp
object->PublishChannel("State");
```

表示对象确认指定通道发生了业务层意义上的变化。它只发布标准的 `DataChannelChanged` 事件，不读取或修改通道数据。

### `PublishEvent`

```cpp
object->PublishEvent(eventType, payload, destroyDataAfterPublish);
```

发布任意系统事件或用户自定义事件。它不负责判断事件是否由数据通道变化引起。

## 3. 公开接口

### 3.1 `IRuntimeObject` 成员函数

```cpp
virtual void PublishEvent(
    RuntimeEventTypeView type,
    IRuntimeObject* data = nullptr,
    bool destroyDataAfterPublish = false) = 0;

virtual void PublishChannel(DataChannelView channel) = 0;
```

`PublishEvent` 是旧 `Publish` 的直接语义重命名。旧名称不保留兼容别名，所有调用方必须迁移到新名称。

`PublishChannel` 是对象自身的成员行为，适用于已有对象指针、引用或透明指针对象的直接调用。

### 3.2 框架层自由函数

```cpp
void PublishChannel(
    IRuntimeObject* source,
    DataChannelView channel);
```

自由函数与成员函数提供相同的通道通知语义，适用于：

- 调用方只持有可能为空的对象指针；
- 通用算法希望使用统一的函数入口；
- 调用代码不希望直接写成员调用；
- 需要在框架工具函数中转发通道变化通知。

自由函数不创建另一套派发逻辑，应直接委托给成员函数：

```cpp
if (source != nullptr) {
    source->PublishChannel(channel);
}
```

因此两种入口的行为始终一致。自由函数不应绕过透明指针的转发规则，也不应直接访问实现类的内部状态。

## 4. `PublishChannel` 的精确定义

调用：

```cpp
object->PublishChannel("State");
```

只表达以下事实：

> 当前对象的 `State` 通道发生了由业务层确认的逻辑变化。

`PublishChannel` 不做以下工作：

- 不调用 `ReadData`；
- 不调用 `WriteData`；
- 不传递通道字节；
- 不比较新旧数据；
- 不判断数据是否真的发生了变化；
- 不替业务层决定通知时机；
- 不自动改变其他对象的通道数据，数据同步由订阅与通道同步机制处理。

其内部效果等价于：

```cpp
DataChannelChangedEventData* payload =
    Runtime::make<DataChannelChangedEventData>(channel);

PublishEvent(
    RuntimeEventTypes::DataChannelChanged,
    payload,
    true);
```

这里的伪代码只用于说明语义，实际实现应复用项目已有的对象创建和事件派发路径。

## 5. 静默无操作规则

以下情况不报错、不抛异常、不发布事件：

- `channel` 为空；
- 成员函数所属对象已经 `Release`；
- 透明指针对象没有绑定目标对象；
- 透明指针对象绑定的目标对象已经 `Release`；
- 自由函数收到空的 `source` 指针。

如果透明指针对象已经绑定有效目标，则：

```cpp
pointerObject->PublishChannel("State");
```

应按照现有透明指针转发规则，将通知转发给绑定对象。自由函数调用该成员函数时保持完全相同的行为。

## 6. 事件载荷所有权

`PublishChannel` 内部创建的 `DataChannelChangedEventData` 仅服务于当前一次同步事件派发，并以独占销毁方式传给 `PublishEvent`：

```cpp
destroyDataAfterPublish = true;
```

因此：

- 同步派发完成后由框架销毁；
- 没有订阅者时也由框架销毁；
- 回调期间可以读取该载荷；
- 回调结束后不得继续使用该载荷；
- 调用方不负责手动 `delete` 该载荷。

普通 `PublishEvent` 的既有事件载荷所有权规则保持不变。

## 7. 指针对象与通道通知

透明指针对象本身不建立独立的事件源和通道存储。它只在已绑定时转发符合规则的操作。

因此：

- 空透明指针调用 `PublishChannel` 时静默无操作；
- 透明指针绑定对象后，成员函数调用转发到绑定对象；
- 透明指针换绑不会自动迁移其他订阅或拓扑关系；
- 自由函数不应自行实现解引用逻辑，而应委托给成员函数，以保证行为一致。

## 8. 调用示例

业务对象先写入数据，再在确认逻辑状态变化后发布通知：

```cpp
const std::uint8_t value[] = {1, 2, 3};
object->WriteData("State", {value, sizeof(value)});
object->PublishChannel("State");
```

发布一般业务事件：

```cpp
object->PublishEvent("HealthChanged", eventData, false);
```

使用自由函数：

```cpp
iobject::PublishChannel(object, "State");
```

自由函数允许安全处理可空指针：

```cpp
iobject::PublishChannel(possiblyNullObject, "State");
```

## 9. 需要同步修改的范围

实施阶段应全项目搜索并更新旧名称及相关说明：

- `include/iobject/IRuntimeObject.hpp`
  - 将 `Publish` 重命名为 `PublishEvent`；
  - 增加 `PublishChannel` 成员函数声明；
  - 增加自由函数声明；
  - 更新接口注释。
- `src/RuntimeObject.cpp`
  - 更新普通运行时对象实现；
  - 更新透明指针对象的事件发布和通道通知转发实现；
  - 更新内部通道同步所使用的事件发布调用。
- `example/03_EventTest.cpp`、`example/06_DataChannelChangeTest.cpp`、`example/07_PointerTest.cpp` 及其他调用点
  - 将 `Publish` 全部改为 `PublishEvent`；
  - 使用 `PublishChannel` 替换手工创建标准通道变化载荷的重复代码。
- `example/08_SubscribeChannelTest.cpp`
  - 删除本地 `PublishChannelChanged` 辅助函数；
  - 使用成员函数或框架层自由函数验证两种入口。
- 规则书和相关说明文档
  - 统一说明 `WriteData`、`PublishChannel` 和 `PublishEvent` 的区别。

## 10. 验证要求

实施后至少验证：

1. 工程可以完整编译；
2. 旧的 `Publish` 名称不再出现在公开接口、实现、示例和相关文档中；
3. 普通事件可以通过 `PublishEvent` 发布；
4. `PublishChannel` 发布的事件类型为 `DataChannelChanged`，且载荷中的通道名正确；
5. 成员函数和自由函数入口行为一致；
6. 空通道、已释放对象、空透明指针和空自由函数参数均静默无操作；
7. 有效透明指针能够将 `PublishChannel` 转发到绑定对象；
8. `PublishChannel` 创建的载荷在派发结束后由框架正确释放；
9. 现有通道订阅、事件递归保护和释放行为不发生回归。

## 11. 非目标

本次设计不包含：

- 自动从 `WriteData` 推断数据变化；
- 自动读取或比较通道数据；
- 改变事件队列或线程模型；
- 新增事件载荷格式；
- 改变通道订阅的拓扑语义；
- 引入远程同步、缓存、合并或异步队列；
- 保留旧 `Publish` 兼容接口。
