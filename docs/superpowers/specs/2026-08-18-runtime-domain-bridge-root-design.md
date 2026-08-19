# RuntimeDomain 与 RuntimeBridgeRoot 第一版远程对象发现设计

> 状态：设计已确认（2026-08-18），尚未实现。本文只定义模型与边界，不引入第三方传输库，不修改当前公开 API。

## 1. 目标与范围

让 JS/远程客户端能够发现并使用 C++ 侧 `IRuntimeObject` 系统中已有的对象：沿拓扑查找对象、读写数据通道、订阅事件。远程端是无感知的使用方——它只面对 `IRuntimeObject` 语义，不关心传输细节；C++ 侧独占拓扑与生命周期管理权。

第一版明确**不做**的事：

- 不引入 WebSocket/Socket 等具体传输实现，只定义桥接模型与接口边界；
- 不允许远程修改拓扑（无 `Connect`/`Disconnect`）、不允许远程释放对象（无 `Release`）；
- 不提供 `As<T>` 给远程端；
- 不做权限管理（默认可读可写）、不做稳定对象 ID 协议（句柄是会话级不透明的）；
- 不实现多域管理器，只定义规则并把现有全局拓扑命名为默认域。

## 2. RuntimeDomain 模型

```text
RuntimeDomainManager（全局，未来）
   └─ RuntimeDomain  1:1  RuntimeTopology
        └─ RuntimeBridgeRoot（唯一，Domain 自动持有）
             └─ 根锚点 IRuntimeObject（Domain 自动创建，纯运行时节点）
                  └─ Connect 向下挂业务对象子树
```

- **域与拓扑一一对应**：每个 `RuntimeDomain` 有且仅有一个 `RuntimeTopology`，有且仅有一个 `RuntimeBridgeRoot`。
- **节点域归属唯一且终身不变**：一个活动中的 `IRuntimeObject` 在同一时刻有且仅属于一个 `RuntimeDomain`，生命周期内不可迁移。当前单域如此，未来多域也定死此规则。
- **第一版实现策略**：只有一个进程内默认域，现有全局 `RuntimeTopology` 即默认域的拓扑。`RuntimeDomain` 先作为概念与设计边界存在，代码层不改动现有公开 API。
- **生命周期顺序**：`RuntimeDomainManager` → `RuntimeDomain`（含根锚点与 `RuntimeBridgeRoot`）→ 业务节点。销毁严格反向。
- **多域预留**：未来 `Connect`、`SubscribeEvent`、`SubscribeChannel`、指针 `Bind` 全部执行同域校验，跨域一律失败；跨域引用由未来的 `ExternalObjectRef` 表达，不进入本域 DAG。

## 3. 根锚点规则

- 根锚点由 `RuntimeDomain` 构造时自动 `Runtime::make()` 创建，是域持有的纯运行时节点；**不由业务方创建或持有**（已确认的方案 2）。
- C++ 业务方从域取得根锚点（如 `domain.RootAnchor()`）后，用既有 `Connect` 把业务对象接入其子树。
- **硬约束（口头约定，不加运行时代码分支）**：根锚点在域及桥接服务存活期间不得 `Release()`、不得 `delete`。违反即未定义使用，框架不承诺会话、发现或句柄的任何行为。
- 因为上一条，第一版**不设计**根锚点释放后的会话关闭、桥接失效、重新绑定等恢复路径；`RuntimeBridgeRoot` 没有运行时替换根锚点的公开操作。
- 正常关闭顺序：停止桥接服务并关闭全部 `RuntimeSession` → 销毁业务对象 → 销毁域（根锚点随之销毁）。

## 4. 远程可见范围

- 远程只能看到从根锚点沿 `Connect` 关系**向下可达**的对象子树；未接入该子树的域内对象远程不可见、不可查。
- 查找只沿现有拓扑进行：`GetChildItem` 语义与本地完全一致（`.` 分隔、逐层向下、不支持 `..`/通配/跨父/全域扫描），只是跨传输层。
- 可见性由"是否接入根锚点子树"决定；**不采用**类静态 `Export()`、对象自声明导出、根对象显式 `Export/Unexport` 注册表（这些方案已在讨论中否决）。

## 5. RuntimeSession 与远程句柄

```text
JS  ConnectRuntime(conn, "MainScene")
      → 服务端校验域存在且根锚点活动
      → 创建 RuntimeSession（传输连接 + 会话级句柄表 + 会话级订阅表）
      → 返回 runtime（含 Root 入口）

多个 RuntimeSession ──多对一──▶ 同一 RuntimeBridgeRoot ──▶ 根锚点子树
```

- **域选择**：第一版连接时显式指定域名（如 `ConnectRuntime(connection, "MainScene")`），服务端精确匹配域。
- **句柄分配**：会话为每个被远程引用的对象分配不透明 `RemoteObjectHandle`（会话内自增 ID）。C++ 内部以 `IRuntimeObject*` 为键；JS 只见到句柄，协议不暴露内存地址。
- **句柄一致性**：同一会话内同一对象无论从哪条拓扑路径到达都返回同一句柄；不同会话的句柄互相独立、不可混用。
- **句柄失效**：对象 `Release` 或析构 → 对应句柄立即失效，后续操作返回"对象不存在"错误；会话关闭 → 该会话全部句柄失效。
- **会话订阅实现**：桥接层为每个会话创建一个私有中继节点（`Runtime::make()` 纯运行时节点，接入域拓扑）作为订阅者，其回调把事件序列化后经传输层推给 JS。这满足"订阅者必须是 `IRuntimeObject`"的既有规则，事件内核零改动。会话关闭时统一 `Release` 并 `delete` 这批中继节点，其所有 `RuntimeSubscription` 随句柄析构自动取消。

## 6. JS 端接口（第一版）

```javascript
const runtime = await ConnectRuntime(connection, "MainScene");
// runtime.Root 是根锚点子树的发现入口，不是根锚点本身的代理，
// 因此不能对 runtime.Root 调用 ReadData / SubscribeEvent 等普通对象接口。

const player  = await runtime.Root.GetChildItem("Player");
const decoder = await runtime.Root.GetChildItem("Player.Decoder");
// 逐级沿现有拓扑解析；任一层不存在 → 明确的对象不存在错误。

await player.ReadData("State");            // 透传到 C++ ReadData
await player.WriteData("State", bytes);    // 透传到 C++ WriteData
const sub = await player.SubscribeEvent("DataChannelChanged", cb);
await sub.Cancel();
await runtime.Close();                     // 关闭会话，句柄与订阅全部失效
```

JS 端接口全集：

| 接口 | 说明 |
| --- | --- |
| `ConnectRuntime(connection, domainName)` | 建立会话；返回的 `runtime` 含 `Root` 入口。 |
| `runtime.Root.GetChildItem(path)` | 沿根锚点子树逐级查找，返回远程对象代理。 |
| `obj.GetChildItem(path)` | 已持有对象继续向下查找。 |
| `obj.ReadData(channel)` / `obj.WriteData(channel, bytes)` | 透传数据通道读写，语义同本地。 |
| `obj.SubscribeEvent(type, handler)` | 订阅事件，返回可 `Cancel()` 的订阅句柄。 |
| `runtime.Close()` | 关闭会话，全部句柄与订阅失效。 |

JS 端**不提供**：`Connect`/`Disconnect`、`Release`、`As<T>`、对 `runtime.Root` 本体的数据/事件操作。要操作根锚点层级的对象，C++ 侧应把业务根作为命名子对象挂在根锚点下。

## 7. 错误与失效语义

- 域不存在或桥接未就绪：`ConnectRuntime` 以明确错误拒绝。
- 路径任一层不存在：`GetChildItem` 返回明确的对象不存在错误（非静默空值）。
- 句柄已失效（对象 Release/析构或会话关闭）：一切对象操作返回"对象不存在"错误。
- 传输断线：会话及其句柄、订阅按会话关闭处理；重连即全新会话，不恢复旧句柄（第一版不做重连恢复）。

## 8. 测试规划（实现时）

- 单元层（无真实传输，用内存回环连接）：
  - 域/根锚点自动创建与销毁顺序；
  - 沿子树 `GetChildItem` 的单层与多级路径、不存在路径的错误；
  - 句柄一致性（多路径到达同一对象）与句柄失效（对象 Release 后操作报错）；
  - 会话关闭后全部句柄与订阅失效；
  - 中继节点订阅：C++ 发布事件后 JS 回调收到对应消息；会话关闭后不再收到。
- 规则层：未接入根锚点子树的域内对象远程不可见。

## 9. 未来演进（仅记录，不在第一版实现）

- 多域管理器 `RuntimeDomainManager` 与跨域 `ExternalObjectRef`；
- 权限管理（两端目前均有完整读写权限）；
- 稳定对象 ID 与协议级身份、重连恢复、初始快照；
- 具体传输实现（WebSocket/Socket/共享内存/串口）与事件循环集成、`DeleteLater`。
