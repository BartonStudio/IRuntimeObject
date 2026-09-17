/** 从解码后的协议字段中取出整数（含 bigint 兜底；协议承诺 <= 2^53）。 */
export function asNumber(value: unknown, name: string): number {
  if (typeof value === "number" && Number.isFinite(value)) {
    return value;
  }
  if (typeof value === "bigint") {
    return Number(value);
  }
  throw new Error(`协议字段 ${name} 应为数字，实际为 ${typeof value}`);
}

/** 从解码后的协议字段中取出二进制（MessagePack bin -> Uint8Array）。 */
export function asBytes(value: unknown, name: string): Uint8Array {
  if (value instanceof Uint8Array) {
    return value;
  }
  throw new Error(`协议字段 ${name} 应为二进制（bin），实际为 ${typeof value}`);
}
