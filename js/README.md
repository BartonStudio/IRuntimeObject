# iobject-js

`IObject` 远程桥接协议（**MessagePack over WebSocket binary**）的 JS/TS 客户端。对应
C++ 侧 `RuntimeSession` + `RuntimeBridgePeer` 的 10 个协议操作。服务端为权威方，本客户端
只能发现对象、读写数据通道、调用方法、订阅事件——不能改拓扑、不能释放对象、没有 `As<T>`。

协议详情见仓库根 `docs/superpowers/specs/2026-08-18-runtime-bridge-messagepack-protocol-design.md`。

## 安装与构建

```bash
cd js
pnpm install      # 或 npm install
pnpm build        # tsc 编译到 dist/
pnpm typecheck    # 仅类型检查
```

运行时依赖只有 `@msgpack/msgpack`；`typescript` 为开发依赖。浏览器与 Node ≥ 22 原生
`WebSocket` 即可，无需 `ws`（Node < 22 见下文）。

## 快速上手

```ts
import { IObjectClient, utf8Encode, utf8Decode } from "iobject-js";

// 连接 + 握手（domain 必须与服务端一致）
const client = await IObjectClient.connect("ws://127.0.0.1:9002", { domain: "iobject" });

const root = client.root;                          // 根锚点
const echo = await root.getChildItem("Echo");      // 单层发现

const version = await echo.readData("Version");    // Uint8Array
await echo.writeData("State", bytes);
const out = await echo.invoke("Echo", utf8Encode("hi")); // Uint8Array

// 订阅事件（信号）：DataChannelChanged 会自带 data 快照
const sub = await echo.subscribe("DataChannelChanged", (ev) => {
  console.log(ev.channel, ev.data);                // ev.data 可选，无则回退 readData 拉取
});
await sub.cancel();

const children = await root.getChildren();         // [{ name, addr, object }]
await client.close();                              // 断开即服务端隐式 Close
```

## API

### `IObjectClient`

- `static connect(url, { domain, WebSocket?, protocols?, openTimeoutMs? }): Promise<IObjectClient>`
  - `domain`：必填，是「域路由键」，须与 C++ 侧 `RuntimeDomain::Name()` 一致（即服务端
    `WebSocketServer::BindDomain(domain)` 注册的那个名字）；
  - `WebSocket`：可选自定义构造器，Node < 22 传 `import WebSocket from "ws"`；
  - `openTimeoutMs`：连接打开超时，默认 10 秒。
- `readonly root: RemoteObject`：根锚点。
- `readonly isOpen: boolean`
- `onClose(listener): () => void`：注册关闭回调，返回注销函数。
- `close(): Promise<void>`：关闭连接（幂等）。
- `request(op, fields)`：低层协议入口（信封 + id 关联），一般业务用 `RemoteObject` 即可。

### `RemoteObject`

| 方法 | 协议 op | 返回 |
| --- | --- | --- |
| `getChildItem(name)` | `GetChildItem` | `RemoteObject` |
| `getChildren()` | `GetChildren` | `{ name, addr, object }[]` |
| `readData(channel)` | `ReadData` | `Uint8Array` |
| `writeData(channel, data)` | `WriteData` | `void` |
| `invoke(method, args)` | `Invoke` | `Uint8Array` |
| `subscribe(type, handler)` | `SubscribeEvent` | `Promise<Subscription>` |

### `Subscription`

- `id`、`isActive`、`cancel(): Promise<void>`（幂等）。

### 错误

所有协议错误抛出 `IObjectError`（`err.code` 为机器可读错误码，`err.message` 为人读文本）：

`MalformedMessage` · `UnknownOp` · `DomainNotFound` · `SessionNotEstablished` ·
`ObjectNotFound` · `AddrInvalid` · `SubscriptionInvalid` · `OperationFailed`

## 与 `tools/ws_client_test.py` 的等价冒烟测试

先启动 C++ 服务端（`example/13_WebSocketHost`，默认 `ws://127.0.0.1:9002`，domain `iobject`）。
注意：`ced053f` 起 `WebSocketServer` 是**独立的系统级传输服务**，由宿主显式创建并
`BindDomain` 注册（不再由 `RuntimeDomain` 自动启动，也不再是对象树里的 `"WebSocket"` 节点），
然后：

```bash
pnpm build
pnpm smoke          # = node example/smoke.mjs
# 或指定地址：IOBJECT_WS_URL=ws://127.0.0.1:9002 node example/smoke.mjs
```

## 协议要点（对接时注意）

- **一条消息 = 一个 WebSocket 二进制帧**；`id`/`addr`/`subscription` 均为无符号整数，协议
  承诺 ≤ 2^53，JS `number` 无损承载，`0` 为无效哨兵。
- **请求**：`{ id, op, ...字段 }`；**响应**：`{ id, ok: true, ...结果 }` 或
  `{ id, ok: false, error: { code, message } }`；**事件**：`{ event, subscription, addr, channel, data? }`（无 `id`）。
- **`addr` 会话作用域**：必须经握手或 `GetChildItem` 在本会话登记后才可用；重连即全新会话，
  旧 `addr` 与 `subscription` 全部作废，需重新发现、重新订阅。
- **方法调用用 `Invoke`**，不要用信号模拟；信号（事件）用于广播通知，二者是正交通道。
- `DataChannelChanged` 事件帧可选携带 `data` 快照（服务端当次 `ReadData` 成功时），无 `data`
  时回退到主动 `readData` 拉取。
