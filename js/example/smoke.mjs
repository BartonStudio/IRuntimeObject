// IObject WebSocket 服务端实测客户端：完整走一遍桥接协议。
// 与 tools/ws_client_test.py 等价。先 `pnpm build`，再 `node example/smoke.mjs`。
// 需 C++ 侧 `example/13_WebSocketHost` 已在运行（默认 ws://127.0.0.1:9002, domain "iobject"）。
import { IObjectClient, utf8Decode, utf8Encode } from "../dist/index.js";

const URL = process.env.IOBJECT_WS_URL ?? "ws://127.0.0.1:9002";
const failures = [];

function check(name, cond, detail = "") {
  const status = cond ? "PASS" : "FAIL";
  console.log(`[${status}] ${name}` + (detail ? `  ${detail}` : ""));
  if (!cond) failures.push(name);
}

function bytesEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

try {
  // 1. 正常握手
  const client = await IObjectClient.connect(URL, { domain: "iobject" });
  const root = client.root;
  check("Connect 握手", root.addr !== 0, `root=${root.addr}`);

  // 2. 枚举根节点：应含 Echo 业务对象
  //    （ced053f 起 WebSocketServer 已是独立传输服务，不再是根锚点下的 "WebSocket" 节点）
  const children = await root.getChildren();
  const names = children.map((c) => c.name);
  check("GetChildren 根节点", names.includes("Echo"), `children=${JSON.stringify(names)}`);

  // 3. 定位 Echo 节点
  const echo = await root.getChildItem("Echo");
  check("GetChildItem Echo", echo.addr !== 0, `addr=${echo.addr}`);

  // 4. ReadData：Version 通道
  const version = await echo.readData("Version");
  check("ReadData Version", utf8Decode(version) === "1.0", `data=${utf8Decode(version)}`);

  // 5. Invoke Echo：客户端 -> 服务端 -> 客户端 双向
  const payload = utf8Encode("hello websocket");
  const out = await echo.invoke("Echo", payload);
  check("Invoke Echo 回显", bytesEqual(out, payload), `result=${utf8Decode(out)}`);

  // 6. 事件订阅 + 服务端主动推送
  const events = [];
  const sub = await echo.subscribe("DataChannelChanged", (ev) => events.push(ev));
  check("SubscribeEvent", sub.isActive, `subscription=${sub.id}`);
  await echo.invoke("Notify", new Uint8Array(0));
  // 事件帧先于 Notify 响应到达（同 socket FIFO），此刻 events 已含该事件。
  check(
    "事件主动推送",
    events.length === 1 && events[0].channel === "Version",
    `event=${JSON.stringify(events.map((e) => ({ event: e.event, channel: e.channel })))}`,
  );
  await sub.cancel();

  // 7. 错误 domain：未注册域名被拒绝
  //    （ced053f 多域路由下服务端直接关 WS 连接，不再发 DomainNotFound 响应帧；
  //      客户端侧表现为连接被拒绝，code 为 OperationFailed 或 DomainNotFound 均可）
  let domainRejected = false;
  try {
    await IObjectClient.connect(URL, { domain: "wrong-domain" });
  } catch (error) {
    domainRejected = error?.code === "OperationFailed" || error?.code === "DomainNotFound";
    console.log(`       (拒绝详情: ${error?.code}: ${error?.message})`);
  }
  check("错误 domain 拒绝", domainRejected);

  // 8. 多客户端并发：第二个客户端独立握手；addr 需在本会话内自行解析 Echo
  const client3 = await IObjectClient.connect(URL, { domain: "iobject" });
  const echo3 = await client3.root.getChildItem("Echo");
  const out3 = await echo3.invoke("Echo", utf8Encode("client-3"));
  check("多客户端并发", utf8Decode(out3) === "client-3");
  await client3.close();

  await client.close();
} catch (error) {
  console.error("[FATAL]", error);
  failures.push(`连接失败: ${error?.message ?? error}`);
}

console.log();
if (failures.length) {
  console.error(`共 ${failures.length} 项失败: ${failures.join(" | ")}`);
  process.exit(1);
}
console.log("全部通过");
