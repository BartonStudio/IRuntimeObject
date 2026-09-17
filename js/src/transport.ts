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
