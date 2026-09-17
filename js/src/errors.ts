/** 协议定义的错误码全集（见 runtime-bridge-messagepack-protocol-design.md 第 6 节）。 */
export const ErrorCodes = {
  MalformedMessage: "MalformedMessage",
  UnknownOp: "UnknownOp",
  DomainNotFound: "DomainNotFound",
  SessionNotEstablished: "SessionNotEstablished",
  ObjectNotFound: "ObjectNotFound",
  AddrInvalid: "AddrInvalid",
  SubscriptionInvalid: "SubscriptionInvalid",
  OperationFailed: "OperationFailed",
} as const;

export type ErrorCode = (typeof ErrorCodes)[keyof typeof ErrorCodes];

/**
 * 服务端返回 `ok: false` 时抛出的错误。
 * `code` 是机器可读错误码（按它分支），`message` 是人读诊断文本（不要解析）。
 */
export class IObjectError extends Error {
  readonly code: string;

  constructor(code: string, message: string) {
    super(message);
    this.name = "IObjectError";
    this.code = code;
  }
}
