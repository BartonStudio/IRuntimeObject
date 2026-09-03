# WebSocket 服务端实测客户端：完整走一遍 IObject 桥接协议（msgpack over ws binary）。
import sys
import msgpack
from websocket import create_connection

URL = "ws://127.0.0.1:9002"
failures = []


def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print(f"[{status}] {name}" + (f"  {detail}" if detail else ""))
    if not cond:
        failures.append(name)


class Client:
    def __init__(self):
        self.ws = create_connection(URL, timeout=5)
        self.next_id = 1
        self.pending = []  # 先到的异步事件帧缓存

    def call(self, op, **fields):
        rid = self.next_id
        self.next_id += 1
        msg = {"id": rid, "op": op}
        msg.update(fields)
        self.ws.send(msgpack.packb(msg), opcode=0x2)  # binary frame
        while True:
            resp = msgpack.unpackb(self.ws.recv(), raw=False)
            if resp.get("id") == rid:
                return resp
            self.pending.append(resp)  # 事件帧可能先于响应到达，缓存而非丢弃

    def recv_event(self):
        if self.pending:
            return self.pending.pop(0)
        return msgpack.unpackb(self.ws.recv(), raw=False)

    def close(self):
        self.ws.close()


# 1. 正常握手
c = Client()
r = c.call("Connect", domain="iobject")
check("Connect 握手", r.get("ok") is True and r.get("root", 0) != 0, f"root={r.get('root')}")
root = r["root"]

# 2. 枚举根节点：应含 Echo 与内置 WebSocket 节点
r = c.call("GetChildren", addr=root)
names = [ch["name"] for ch in r.get("children", [])]
check("GetChildren 根节点", r.get("ok") and "Echo" in names and "WebSocket" in names,
      f"children={names}")

# 3. 定位 Echo 节点
r = c.call("GetChildItem", addr=root, childId="Echo")
check("GetChildItem Echo", r.get("ok") and r.get("addr", 0) != 0, f"addr={r.get('addr')}")
echo = r["addr"]

# 4. ReadData：Version 通道
r = c.call("ReadData", addr=echo, channel="Version")
data = bytes(r.get("data", b"")).decode(errors="replace")
check("ReadData Version", r.get("ok") and data == "1.0", f"data={data!r}")

# 5. Invoke Echo：客户端 -> 服务端 -> 客户端 双向
payload = b"hello websocket"
r = c.call("Invoke", addr=echo, method="Echo", args=payload)
result = bytes(r.get("result", b""))
check("Invoke Echo 回显", r.get("ok") and result == payload, f"result={result!r}")

# 6. 事件订阅 + 服务端主动推送
r = c.call("SubscribeEvent", addr=echo, type="DataChannelChanged")
check("SubscribeEvent", r.get("ok") and r.get("subscription", 0) != 0,
      f"subscription={r.get('subscription')}")
c.call("Invoke", addr=echo, method="Notify", args=b"")
ev = c.recv_event()
check("事件主动推送", ev.get("event") == "DataChannelChanged" and ev.get("channel") == "Version",
      f"event={ev}")

# 7. 错误 domain：应被拒绝
c2 = Client()
r = c2.call("Connect", domain="wrong-domain")
check("错误 domain 拒绝", r.get("ok") is False and r.get("error", {}).get("code") == "DomainNotFound",
      f"resp={r}")
c2.close()

# 8. 多客户端并发：第二个客户端独立握手；addr 是 session 作用域，需自行解析 Echo
c3 = Client()
r = c3.call("Connect", domain="iobject")
ok3 = r.get("ok") is True
r = c3.call("GetChildItem", addr=r["root"], childId="Echo")
echo3 = r["addr"]
r = c3.call("Invoke", addr=echo3, method="Echo", args=b"client-3")
check("多客户端并发", ok3 and bytes(r.get("result", b"")) == b"client-3")
c3.close()

# 9. 内置 WebSocket 节点的 Port 通道（大端 uint16）
r = c.call("GetChildItem", addr=root, childId="WebSocket")
wsnode = r["addr"]
r = c.call("ReadData", addr=wsnode, channel="Port")
port = int.from_bytes(bytes(r.get("data", b"")), "big")
check("WebSocket 节点 Port 通道", r.get("ok") and port == 9002, f"port={port}")

c.close()
print()
if failures:
    print(f"共 {len(failures)} 项失败: {failures}")
    sys.exit(1)
print("全部通过")
