import type { SubscriptionId } from "./protocol.js";
import type { IObjectClient } from "./client.js";

/** 事件订阅句柄：`cancel()` 幂等解除订阅。 */
export class Subscription {
  private active = true;

  constructor(
    private readonly client: IObjectClient,
    readonly id: SubscriptionId,
  ) {}

  get isActive(): boolean {
    return this.active;
  }

  async cancel(): Promise<void> {
    if (!this.active) {
      return;
    }
    this.active = false;
    await this.client.cancelEvent(this.id);
  }
}
