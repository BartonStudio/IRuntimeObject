/**
 * 协议类型定义。协议承诺所有无符号整数（id / addr / subscription）不超过 2^53，
 * 因此 JS `number` 即可无损承载，无需 BigInt。
 */

/** 对象标识 addr：对象指针的数值形式，同对象跨会话同值；0 为无效哨兵。 */
export type Addr = number;

/** 订阅 ID：会话内唯一，客户端据此精确分发事件帧。 */
export type SubscriptionId = number;

/** 请求 ID：客户端自选，会话内唯一即可，用于关联响应。 */
export type RequestId = number;

/** 协议操作全集（10 个：握手 Connect 加 9 个会话操作）。 */
export type Op =
  | "Connect"
  | "GetChildItem"
  | "GetChildren"
  | "ReadData"
  | "WriteData"
  | "Invoke"
  | "SubscribeEvent"
  | "CancelEvent"
  | "Close";

/**
 * 下行事件帧。
 * `data` 仅当事件为 `DataChannelChanged` 且服务端当次 `ReadData` 成功时存在
 * （字节快照；空字节合法，判断字段存在性而非长度）。其余事件或读取失败时不存在，
 * 客户端应回退到主动 `ReadData` 拉取。
 */
export interface IObjectEvent {
  event: string;
  subscription: SubscriptionId;
  addr: Addr;
  channel: string;
  data?: Uint8Array;
}

export type EventHandler = (event: IObjectEvent) => void;
