import { decode, encode } from "@msgpack/msgpack";

import type {
  Addr,
  EventHandler,
  IObjectEvent,
  Op,
  RequestId,
  SubscriptionId,
} from "./protocol.js";
import { IObjectError } from "./errors.js";
import { RemoteObject } from "./remote-object.js";
import { Subscription } from "./subscription.js";
import {
  defaultWebSocketCtor,
  toUint8Array,
  type WebSocketCtor,
  type WebSocketLike,
} from "./transport.js";
import { asNumber } from "./values.js";

export interface ConnectOptions {
  /** 连接对应的域名；须与服务端构造 RuntimeBridgePeer 时传入的 domain 一致。 */
  domain: string;
  /** 可选自定义 WebSocket 构造器（Node < 22 传 `ws` 包导出）。默认 globalThis.WebSocket。 */
  WebSocket?: WebSocketCtor;
  /** 可选：传给 WebSocket 构造器的子协议。 */
  protocols?: string | string[];
  /** 可选：连接打开超时（毫秒），默认 10 秒。 */
  openTimeoutMs?: number;
}

interface Pending {
  resolve: (value: Record<string, unknown>) => void;
  reject: (reason: unknown) => void;
}

/** 解码后的响应/事件帧（宽松的 map）。 */
type Frame = Record<string, unknown>;

/**
 * IObject 远程桥接客户端。一个实例对应一个 RuntimeSession（一条连接）。
 * 连接建立后 `root` 是根锚点，沿 `getChildItem` 逐级发现对象。
 */
export class IObjectClient {
  /** 根锚点；握手成功后可用。 */
  readonly root!: RemoteObject;

  private readonly ws: WebSocketLike;
  private nextId: RequestId = 1;
  private readonly pending = new Map<RequestId, Pending>();
  private readonly handlers = new Map<SubscriptionId, EventHandler>();
  private readonly closeListeners: Array<() => void> = [];
  private closed = false;
  private opened = false;
  private openResolvers?: { resolve: () => void; reject: (reason: unknown) => void };

  private constructor(
    url: string,
    private readonly opts: Required<ConnectOptions>,
  ) {
    const Ctor = opts.WebSocket;
    this.ws = new Ctor(url, opts.protocols);
    try {
      this.ws.binaryType = "arraybuffer";
    } catch {
      // 某些实现不暴露 binaryType，忽略；onMessage 侧再统一归一化。
    }
    this.ws.onmessage = (event) => this.onMessage(event.data);
    this.ws.onclose = (event) => this.handleSocketClose(event);
    this.ws.onerror = () => {
      // 错误后通常紧跟 onclose，由 onClose 统一收尾；这里兜底未打开即失败的情况。
      if (!this.opened) {
        this.openResolvers?.reject(new IObjectError("OperationFailed", "WebSocket 连接错误"));
      }
    };
  }

  /** 建立连接并完成 Connect 握手，返回已就绪的客户端。 */
  static async connect(url: string, options: ConnectOptions): Promise<IObjectClient> {
    const opts: Required<ConnectOptions> = {
      domain: options.domain,
      WebSocket: options.WebSocket ?? defaultWebSocketCtor(),
      protocols: options.protocols ?? [],
      openTimeoutMs: options.openTimeoutMs ?? 10_000,
    };
    const client = new IObjectClient(url, opts);
    try {
      await client.waitOpen();
      const res = await client.request("Connect", { domain: opts.domain });
      const rootAddr = asNumber(res.root, "root");
      if (rootAddr === 0) {
        throw new IObjectError("OperationFailed", "握手成功但根锚点 addr 为 0");
      }
      (client as { root: RemoteObject }).root = new RemoteObject(client, rootAddr);
      return client;
    } catch (error) {
      // 握手失败（如 DomainNotFound）时关闭底层 socket，避免悬挂句柄拖住事件循环。
      client.ws.close();
      client.finalize(
        error instanceof IObjectError ? error : new IObjectError("OperationFailed", String(error)),
      );
      throw error;
    }
  }

  get isOpen(): boolean {
    return !this.closed;
  }

  /** 注册连接关闭回调，返回注销函数。 */
  onClose(listener: () => void): () => void {
    this.closeListeners.push(listener);
    return () => {
      const index = this.closeListeners.indexOf(listener);
      if (index >= 0) {
        this.closeListeners.splice(index, 1);
      }
    };
  }

  /** 关闭连接（传输断开即服务端隐式 Close；幂等）。 */
  async close(): Promise<void> {
    if (this.closed) {
      return;
    }
    this.ws.close();
    this.finalize(new IObjectError("OperationFailed", "连接已主动关闭"));
  }

  /** 订阅事件，返回可取消句柄。 */
  async subscribe(addr: Addr, type: string, handler: EventHandler): Promise<Subscription> {
    const res = await this.request("SubscribeEvent", { addr, type });
    const subscription = asNumber(res.subscription, "subscription");
    if (subscription === 0) {
      throw new IObjectError("OperationFailed", "订阅失败");
    }
    this.handlers.set(subscription, handler);
    return new Subscription(this, subscription);
  }

  /** 取消订阅（由 Subscription.cancel 调用）。 */
  async cancelEvent(subscription: SubscriptionId): Promise<void> {
    this.handlers.delete(subscription);
    await this.request("CancelEvent", { subscription });
  }

  /** 低层请求入口：编码信封 -> 发送 -> 按 id 关联响应。 */
  async request(op: Op, fields: Record<string, unknown> = {}): Promise<Frame> {
    if (this.closed) {
      throw new IObjectError("SessionNotEstablished", "会话未建立或已关闭");
    }
    const id = this.nextId++;
    const message: Record<string, unknown> = { id, op, ...fields };
    return new Promise<Frame>((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      try {
        this.ws.send(encode(message));
      } catch (error) {
        this.pending.delete(id);
        reject(error);
      }
    });
  }

  private waitOpen(): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => {
        reject(new IObjectError("OperationFailed", `连接打开超时（${this.opts.openTimeoutMs}ms）`));
      }, this.opts.openTimeoutMs);
      this.openResolvers = {
        resolve: () => {
          clearTimeout(timer);
          resolve();
        },
        reject: (reason) => {
          clearTimeout(timer);
          reject(reason);
        },
      };
      this.ws.onopen = () => {
        this.opened = true;
        this.openResolvers?.resolve();
      };
    });
  }

  private onMessage(data: unknown): void {
    let message: unknown;
    try {
      message = decode(toUint8Array(data));
    } catch (error) {
      console.error("[iobject-js] 无法解码 MessagePack 帧", error);
      return;
    }
    if (message === null || typeof message !== "object" || Array.isArray(message)) {
      console.error("[iobject-js] 收到非对象帧");
      return;
    }
    const frame = message as Frame;
    // 响应帧带 id；事件帧带 event（无 id）。
    if (typeof frame.id === "number") {
      this.onResponse(frame);
    } else if (typeof frame.event === "string") {
      this.onEvent(frame);
    } else {
      console.error("[iobject-js] 无法识别的帧", frame);
    }
  }

  private onResponse(frame: Frame): void {
    const id = asNumber(frame.id, "id");
    const pending = this.pending.get(id);
    if (!pending) {
      return; // 超时或已取消，忽略。
    }
    this.pending.delete(id);
    if (frame.ok === true) {
      pending.resolve(frame);
      return;
    }
    const error = frame.error as { code?: unknown; message?: unknown } | undefined;
    pending.reject(
      new IObjectError(
        typeof error?.code === "string" ? error.code : "OperationFailed",
        typeof error?.message === "string" ? error.message : "未知错误",
      ),
    );
  }

  private onEvent(frame: Frame): void {
    const subscription = asNumber(frame.subscription, "subscription");
    const handler = this.handlers.get(subscription);
    if (!handler) {
      return;
    }
    const event: IObjectEvent = {
      event: frame.event as string,
      subscription,
      addr: asNumber(frame.addr, "addr"),
      channel: typeof frame.channel === "string" ? frame.channel : "",
      data: frame.data instanceof Uint8Array ? frame.data : undefined,
    };
    try {
      handler(event);
    } catch (error) {
      console.error("[iobject-js] 事件处理器抛出异常", error);
    }
  }

  private handleSocketClose(event: { code?: number; reason?: string }): void {
    const reason = event?.reason || `连接关闭（code=${event?.code ?? "?"}）`;
    this.finalize(new IObjectError("OperationFailed", reason));
  }

  private finalize(error: IObjectError): void {
    if (this.closed) {
      return;
    }
    this.closed = true;
    this.openResolvers?.reject(error); // 已 resolve 则为无操作。
    for (const pending of this.pending.values()) {
      pending.reject(error);
    }
    this.pending.clear();
    this.handlers.clear();
    const listeners = this.closeListeners.splice(0);
    for (const listener of listeners) {
      listener();
    }
  }
}
