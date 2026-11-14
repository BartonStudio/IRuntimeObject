/**
 * 极简 WebSocket 抽象：兼容浏览器原生 WebSocket 与 Node 的 `ws` 包。
 * 库本身不依赖具体实现，只依赖这里的最小结构。
 */
export interface WebSocketLike {
  binaryType?: string;
  onopen: ((event: unknown) => void) | null;
  onmessage: ((event: { data: unknown }) => void) | null;
  onclose: ((event: { code?: number; reason?: string }) => void) | null;
  onerror: ((event: unknown) => void) | null;
  send(data: Uint8Array | ArrayBuffer): void;
  close(code?: number, reason?: string): void;
}

export type WebSocketCtor = new (url: string, protocols?: string | string[]) => WebSocketLike;

/** 默认取运行环境的全局 WebSocket（浏览器或 Node >= 22）。 */
export function defaultWebSocketCtor(): WebSocketCtor {
  const ctor = (globalThis as { WebSocket?: WebSocketCtor }).WebSocket;
  if (!ctor) {
    throw new Error(
      "当前环境无全局 WebSocket：Node < 22 请在 connect 选项传入 ws 包（import WebSocket from 'ws'）",
    );
  }
  return ctor;
}

/** 把 WebSocket 消息数据统一为 Uint8Array（ArrayBuffer / TypedArray / Buffer 均可）。 */
export function toUint8Array(data: unknown): Uint8Array {
  if (data instanceof Uint8Array) {
    return data;
  }
  if (data instanceof ArrayBuffer) {
    return new Uint8Array(data);
  }
  if (ArrayBuffer.isView(data)) {
    return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  }
  throw new Error("收到非二进制 WebSocket 帧（协议要求 binary frame）");
}

/** WebView2 的 window.chrome.webview 宿主对象（只用到 postMessage / postMessageWithAdditionalObjects / addEventListener）。 */
export interface WebviewHost {
  postMessage(message: unknown, additionalObjects?: unknown[]): void;
  postMessageWithAdditionalObjects(message: unknown, additionalObjects?: unknown[]): void;
  addEventListener(type: string, listener: (event: unknown) => void): void;
  removeEventListener(type: string, listener: (event: unknown) => void): void;
}

/** sharedbufferreceived 事件对象：getBuffer() 返回下行的 ArrayBuffer。 */
interface SharedBufferReceivedEvent {
  getBuffer(): ArrayBuffer;
  additionalData?: { type?: string };
}

function webviewHost(): WebviewHost | null {
  const g = globalThis as { chrome?: { webview?: WebviewHost } };
  return g.chrome?.webview ?? null;
}

/**
 * 创建一条跑在 WebView2 内进程通道上的 WebSocketLike 传输：
 *   上行 send()  → 写入 native 预推的 READ_WRITE 共享 buffer，再 postMessage({type:"iobject",offset,len}) 通知 C++ 读
 *   下行         → chrome.webview 'sharedbufferreceived' 事件（type:"iobject"）→ onmessage
 *
 * open 语义：等 native 推来上行 buffer（iobject-uplink）后才触发 onopen，
 * 让 IObjectClient 的 waitOpen 在上行通道就绪后再发 Connect。close 只清监听并回调 onclose。
 */
export function webviewSocket(): WebSocketLike {
  const host = webviewHost();
  if (!host) {
    throw new Error("当前环境无 window.chrome.webview：WebView 桥接仅在内嵌页面可用");
  }

  let uplinkBuffer: ArrayBuffer | null = null;
  let uplinkOffset = 0;

  const socket: WebSocketLike = {
    binaryType: "arraybuffer",
    onopen: null,
    onmessage: null,
    onclose: null,
    onerror: null,

    send(data: Uint8Array | ArrayBuffer): void {
      const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
      if (!uplinkBuffer) {
        throw new Error("上行共享 buffer 未就绪（尚未收到 iobject-uplink）");
      }
      // append-only 写偏移，溢出回绕（iobject 帧远小于 4MB，串行场景安全）。
      if (uplinkOffset + bytes.length > uplinkBuffer.byteLength) {
        uplinkOffset = 0;
      }
      new Uint8Array(uplinkBuffer, uplinkOffset, bytes.length).set(bytes);
      // 文本通知 native 读 offset/len（WebView2 不支持 postMessageWithAdditionalObjects 传普通 ArrayBuffer）。
      host.postMessage({ type: "iobject", offset: uplinkOffset, len: bytes.length });
      uplinkOffset += bytes.length;
    },

    close(): void {
      host.removeEventListener("sharedbufferreceived", onSharedBufferReceived);
      socket.onclose?.({ code: 1000, reason: "closed" });
    },
  };

  function onSharedBufferReceived(event: unknown): void {
    const e = event as SharedBufferReceivedEvent;
    // 上行 buffer（READ_WRITE）：JS 写 → C++ 读。收到后保存并触发 onopen（上行通道就绪）。
    if (e.additionalData?.type === "iobject-uplink") {
      uplinkBuffer = e.getBuffer();
      uplinkOffset = 0;
      socket.onopen?.({});
      return;
    }
    // 下行帧（C++ 推的 msgpack 响应 / 事件帧）。
    if (e.additionalData?.type === "iobject") {
      socket.onmessage?.({ data: e.getBuffer() });
    }
  }

  host.addEventListener("sharedbufferreceived", onSharedBufferReceived);
  // 请求 native 预推一块 READ_WRITE 上行 buffer（native 响应走 sharedbufferreceived）。
  host.postMessage({ type: "iobject-init" });

  return socket;
}
