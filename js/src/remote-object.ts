import type { Addr, EventHandler } from "./protocol.js";
import type { IObjectClient } from "./client.js";
import type { Subscription } from "./subscription.js";
import { asBytes, asNumber } from "./values.js";

export interface ChildEntry {
  name: string;
  addr: Addr;
  object: RemoteObject;
}

/**
 * 远程对象的薄封装：持有会话内已登记的 addr，把协议操作翻译为语义化方法。
 * 远端不能 Connect / Disconnect / Release / As<T>，因此这里只暴露发现、读写、调用、订阅。
 */
export class RemoteObject {
  constructor(
    private readonly client: IObjectClient,
    readonly addr: Addr,
  ) {}

  /** 解析当前对象的直接子对象（childId 是非空、不含 `.` 的单层名称）。 */
  async getChildItem(childId: string): Promise<RemoteObject> {
    const res = await this.client.request("GetChildItem", { addr: this.addr, childId });
    return new RemoteObject(this.client, asNumber(res.addr, "addr"));
  }

  /** 枚举当前对象的全部直接子节点（只读发现）。 */
  async getChildren(): Promise<ChildEntry[]> {
    const res = await this.client.request("GetChildren", { addr: this.addr });
    const raw = res.children;
    if (!Array.isArray(raw)) {
      return [];
    }
    return raw.map((child) => {
      const item = child as { name?: unknown; addr?: unknown };
      const name = typeof item.name === "string" ? item.name : "";
      const addr = asNumber(item.addr, "addr");
      return { name, addr, object: new RemoteObject(this.client, addr) };
    });
  }

  /** 同步读取数据通道，返回不透明字节。 */
  async readData(channel: string): Promise<Uint8Array> {
    const res = await this.client.request("ReadData", { addr: this.addr, channel });
    return asBytes(res.data, "data");
  }

  /** 同步写入数据通道（不透明字节）。 */
  async writeData(channel: string, data: Uint8Array): Promise<void> {
    await this.client.request("WriteData", { addr: this.addr, channel, data });
  }

  /** 调用对象暴露的命名方法（命令/动作），返回不透明结果字节。 */
  async invoke(method: string, args: Uint8Array): Promise<Uint8Array> {
    const res = await this.client.request("Invoke", { addr: this.addr, method, args });
    return asBytes(res.result, "result");
  }

  /** 订阅对象事件，返回可取消句柄。 */
  subscribe(type: string, handler: EventHandler): Promise<Subscription> {
    return this.client.subscribe(this.addr, type, handler);
  }
}
