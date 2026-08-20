# RuntimeBridge MessagePack 消息协议设计

> 状态：设计已确认（2026-08-18），尚未实现。本文定义客户端（JS）与服务端（C++ `RuntimeSession`）之间的消息格式；传输实现与编解码库选型不在本文范围。

## 1. 定位与边界

本协议是 `RuntimeSession` 方法集的消息化包装，遵循 `2026-08-18-runtime-domain-bridge-root-design.md` 的模型：远程端沿根锚点向下发现对象、读写数据通道、订阅事件；不能修改拓扑、不能释放对象、没有 `As<T>`。

- **编码**：全部消息使用 MessagePack。选它的原因：schema-less、原生 bin 类型（不透明字节通道零膨胀）、有成熟 JS 库。不使用 protobuf（schema 驱动与不透明通道模型冲突），不使用 JSON+Base64（数据膨胀）。
- **无版本概念**：协议不定义版本字段，不做版本兼容协商。消息中未识别的字段一律忽略（防御性解析，非版本机制）。
- **对象标识 `addr`**：协议中对象的唯一标识，对应 C++ 侧 `RuntimeSession` 的 `RemoteObjectHandle` 值——会话内分配的不透明 64 位编号，会话内唯一，跨会话不可混用。`Connect` 成功时返回根锚点的 `addr`，之后一切对象操作都以 `addr` 指定目标。

## 2. 帧规则

一条消息 = 一个自包含的 MessagePack 文档（map）。分包是传输层的职责，标准映射：

- **WebSocket**：一条消息 = 一个二进制帧（首选承载，天然分包）；
- **流式传输**（TCP/串口等，未来）：4 字节大端长度前缀 + MessagePack 文档。

单条消息建议不超过 1 MiB；超过视为 `MalformedMessage` 并可断开连接。具体阈值由传输实现决定，协议只给建议值。

## 3. 信封规则

- 每条消息是一个 MessagePack **map**。
- **上行请求**（客户端→服务端）必含：
  - `op`：字符串，操作名；
  - `id`：无符号整数，客户端自选，用于关联响应；会话内唯一即可，可自增复用。
- **下行响应**（服务端→客户端）必含：
  - `id`：回显请求的 `id`；
  - `ok`：布尔。`true` 时携带结果字段；`false` 时携带 `error`。
- **下行事件**（服务端→客户端）不含 `id`，含 `event` 字段，借此与响应区分。
- `addr`、`subscription`、`id` 均为无符号整数，**协议承诺不超过 2^53**（JS Number 精度上限）。`0` 保留为无效值，正常消息中不出现——子对象未命中、addr 失效等情况一律用 `ok: false` 表达。唯一例外：`MalformedMessage` 响应在无法从畸形请求中提取 `id` 时以 `id: 0` 回应（见第 6 节）。

## 4. 操作消息全集

8 个操作：握手 `Connect` 加 7 个会话操作。

### 4.1 握手

连接后的第一条消息必须是 `Connect`；成功响应返回根锚点的 `addr`：

```text
→ { "op": "Connect", "id": 1, "domain": "MainScene" }
← { "id": 1, "ok": true, "root": 1745238901234561 }
```

`domain` 不存在时回 `DomainNotFound`，随后服务端关闭连接。

根锚点是可寻址对象：远程可对根 `SubscribeEvent`（例如订阅 `ChildConnected` 感知 C++ 侧新接入的对象）；对根 `ReadData`/`WriteData` 自然失败（纯运行时节点无通道能力）；协议没有 `Connect`/`Disconnect`/`Release` 操作，远程无法修改拓扑或释放任何对象。

### 4.2 对象发现

`GetChildItem` 按**单层**子名称解析：`childId` 是 `addr` 指定对象的直接子对象名称（非空、不含 `.`）。多级导航由客户端逐级调用完成；从根锚点的 `addr` 出发即可发现其下任意后代。成功响应回显 `childId` 并返回子对象的 `addr`：

```text
→ { "op": "GetChildItem", "id": 2, "addr": 1745238901234561, "childId": "Player" }
← { "id": 2, "ok": true, "childId": "Player", "addr": 1745238901234562 }

→ { "op": "GetChildItem", "id": 3, "addr": 1745238901234562, "childId": "Decoder" }
← { "id": 3, "ok": true, "childId": "Decoder", "addr": 1745238901234563 }
```

子对象不存在回 `ObjectNotFound`；`addr` 无效回 `AddrInvalid`。

### 4.3 数据通道

`data` 是 MessagePack bin 类型，原生字节：

```text
→ { "op": "ReadData", "id": 4, "addr": 1745238901234563, "channel": "State" }
← { "id": 4, "ok": true, "data": <bin> }

→ { "op": "WriteData", "id": 5, "addr": 1745238901234563, "channel": "State", "data": <bin> }
← { "id": 5, "ok": true }
```

错误区分依赖 `RuntimeSession` 新增的 addr 有效性查询（见第 8 节）：适配器先检查 `addr`，无效回 `AddrInvalid`；有效但 `ReadData`/`WriteData` 返回 `false`（对象拒绝、未知通道等）回 `OperationFailed`。

### 4.4 事件订阅

```text
→ { "op": "SubscribeEvent", "id": 6, "addr": 1745238901234563, "type": "DataChannelChanged" }
← { "id": 6, "ok": true, "subscription": 864197523 }

→ { "op": "CancelEvent", "id": 7, "subscription": 864197523 }
← { "id": 7, "ok": true }
```

同样先做 addr 有效性检查：无效回 `AddrInvalid`；有效但 `RuntimeSession::SubscribeEvent` 返回 0 回 `OperationFailed`；`CancelEvent` 的 `subscription` 无效或已取消回 `SubscriptionInvalid`。

### 4.5 关闭

```text
→ { "op": "Close", "id": 8 }
← { "id": 8, "ok": true }
```

响应后服务端关闭会话与连接。传输层断开等价于隐式 `Close`。

## 5. 事件下行帧

服务端主动推送，无 `id`：

```text
← { "event": "DataChannelChanged", "subscription": 864197523, "addr": 1745238901234563, "channel": "State", "data": <bin> }
```

- `event`：事件类型字符串，与 C++ 侧完全一致（内置常量或业务自定义）；
- `subscription`：订阅 ID，客户端凭它精确分发到注册的回调，同对象同类型多次订阅不混淆；
- `addr`：事件源对象的 addr（`RemoteEventMessage.source`）；
- `channel`：始终存在；仅 `DataChannelChanged` 事件为非空（载荷可 `As<DataChannelChangedEventData>()` 时取其通道名），其余事件为空字符串；
- `data`：**可选**——`DataChannelChanged` 事件在派发当次对源对象 `ReadData` 该通道成功时携带的字节快照（MessagePack bin，空字节合法，判断字段存在性而非长度）。读取失败或其他事件类型时不携带，客户端应回退到主动 `ReadData` 拉取。快照在每次远程订阅的事件上都会产生（无按需开关），高频大流量通道需留意开销。
- 没有订阅者的事件不产生任何帧。
- 对象 `Release` 时，已订阅的客户端收到 `{ "event": "Released", ... }`，此后该 addr 的一切操作回 `AddrInvalid`。

## 6. 错误格式

```text
← { "id": 4, "ok": false, "error": { "code": "AddrInvalid", "message": "addr 1745238901234563 已失效或不存在" } }
```

- `code`：机器可读字符串，客户端按它分支；
- `message`：人读诊断文本，客户端不得解析。

错误码全集：

| code | 触发场景 |
| --- | --- |
| `MalformedMessage` | 消息不是合法信封（非 map、缺 `op`/`id`、字段类型错误、超长）；能提取 `id` 则回显，否则以 `id: 0` 回应 |
| `UnknownOp` | `op` 不存在 |
| `DomainNotFound` | `Connect` 的 `domain` 不存在 |
| `SessionNotEstablished` | 握手成功前发送了其他 op |
| `ObjectNotFound` | 子对象名称未命中 |
| `AddrInvalid` | addr 无效、已失效或不属于本会话 |
| `SubscriptionInvalid` | 订阅 ID 无效或已取消 |
| `OperationFailed` | 对象操作返回失败（`ReadData`/`WriteData`/`SubscribeEvent` 等） |

## 7. 连接生命周期

```text
连接建立 → Connect 握手（获得根 addr）→ 会话可用（正常收发）→ Close 或传输断开 → 会话终结
```

- 握手前只接受 `Connect`，其他 op 回 `SessionNotEstablished`；
- 会话终结后全部 addr 与订阅失效，服务端不再发送任何帧；
- 重连即全新会话，旧 addr 与订阅不可复用（第一版不做重连恢复）。

## 8. 实现分层（实现时的结构）

```text
传输层（WebSocket 等，第三方库）
   ↕ MessagePack 文档
协议适配器（新组件：解析/构造消息，错误映射）
   ↕ C++ 调用 / RemoteEventCallback
RuntimeSession（已实现）
```

协议适配器把请求消息翻译成 `RuntimeSession` 方法调用（`addr` 即 `RemoteObjectHandle`），把 `RemoteEventCallback` 收到的 `RemoteEventMessage` 翻译成事件下行帧。`GetChildItem` 的 `childId` 是单层名称，适配器直接以单段路径调用会话接口。

`RuntimeSession` 仅需一处小增补：addr 有效性查询（如 `bool HasObject(RemoteObjectHandle) const`），供适配器区分 `AddrInvalid` 与 `OperationFailed`。另外 `RuntimeBridgeRoot` 需要在会话建立时把根锚点注册进会话并返回其 addr（`Connect` 响应的 `root` 字段来源）。其余会话逻辑不变，不含任何传输与协议知识。

## 9. 测试规划（实现时）

- 用内存回环传输（不进网络）驱动协议适配器：编码→解码→调用→响应编码的全链路往返；
- 逐操作覆盖：握手成功（返回根 addr）/域不存在、从根 addr 逐级 `GetChildItem` 解析、未命中、读写成功与拒绝、订阅/取消、事件帧字段、Released 下行、关闭后会话终结；
- 畸形消息：非 map、缺字段、类型错误、超长消息；
- 2^53 约束只作文档承诺，不做运行时测试。

## 10. 实现记录

- 编解码库：msgpack11（`ar90n/msgpack11`，MIT），vendored 于 `third_party/msgpack11/`，随 IObject 静态库编译。
- 重复 `Connect`（会话已建立后再次握手）返回 `OperationFailed`，规格第 4.1 节未覆盖此情形，以本节为准。
- `childId` 为空或含 `.` 时返回 `MalformedMessage`（协议规定它是单层名称）。
- 对象 `Release` 后被内核自动取消的订阅，之后 `CancelEvent` 返回 ok（幂等从简）；`SubscriptionInvalid` 只覆盖从未存在或已被显式取消的 ID。
- 单条消息上限按建议值实现为 1 MiB，超过回 `MalformedMessage`（`id: 0`）并关闭连接。
- 事件帧 `channel` 字段仅 `DataChannelChanged` 非空；此类事件在派发当次对源对象 `ReadData` 成功时携带 `data` 字节快照（已实现），通用事件载荷的传输仍未实现。
