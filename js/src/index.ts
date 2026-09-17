export { IObjectClient } from "./client.js";
export type { ConnectOptions } from "./client.js";

export { RemoteObject } from "./remote-object.js";
export type { ChildEntry } from "./remote-object.js";

export { Subscription } from "./subscription.js";

export { IObjectError, ErrorCodes } from "./errors.js";
export type { ErrorCode } from "./errors.js";

export type {
  Addr,
  SubscriptionId,
  RequestId,
  Op,
  IObjectEvent,
  EventHandler,
} from "./protocol.js";

export { defaultWebSocketCtor, toUint8Array } from "./transport.js";
export type { WebSocketLike, WebSocketCtor } from "./transport.js";

/** UTF-8 编码字符串为字节（数据通道 / 方法参数常用）。 */
export const utf8Encode = (text: string): Uint8Array => new TextEncoder().encode(text);

/** UTF-8 解码字节为字符串。 */
export const utf8Decode = (bytes: Uint8Array): string => new TextDecoder().decode(bytes);
