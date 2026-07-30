import type { SerialTransport } from "./transport";

/**
 * Buffers chunks from a SerialTransport and exposes line- and
 * exact-byte-count reads, mirroring tools/devctl.py's `_read_line` /
 * `_read_exact` but pull-based over a chunked transport instead of
 * byte-by-byte serial reads.
 */
export class LineReader {
  private buffer: Uint8Array = new Uint8Array(0);
  private closed = false;

  constructor(private readonly transport: SerialTransport) {}

  private append(chunk: Uint8Array): void {
    const merged = new Uint8Array(this.buffer.length + chunk.length);
    merged.set(this.buffer, 0);
    merged.set(chunk, this.buffer.length);
    this.buffer = merged;
  }

  private async fill(): Promise<void> {
    const chunk = await this.transport.read();
    if (chunk === null) {
      this.closed = true;
      return;
    }
    this.append(chunk);
  }

  /**
   * Reads and returns the next LF-terminated line, decoded as UTF-8, with a
   * trailing CR stripped (matches devctl.py). The device's log lines and
   * `@`-prefixed replies interleave on the same stream; callers that only
   * want control replies should skip non-`@` lines themselves (see
   * DeviceClient.readControlLine).
   */
  async readLine(): Promise<string> {
    for (;;) {
      const nl = this.buffer.indexOf(0x0a);
      if (nl !== -1) {
        let end = nl;
        if (end > 0 && this.buffer[end - 1] === 0x0d) end--;
        const line = new TextDecoder().decode(this.buffer.subarray(0, end));
        this.buffer = this.buffer.subarray(nl + 1);
        return line;
      }
      if (this.closed) throw new Error("connection closed while waiting for a line");
      await this.fill();
    }
  }

  /** Reads exactly `n` raw bytes, e.g. the pixel/file payload after a `@shot`/`@fget` header. */
  async readExact(n: number): Promise<Uint8Array> {
    while (this.buffer.length < n) {
      if (this.closed) {
        throw new Error(
          `connection closed after ${this.buffer.length} of ${n} expected bytes`,
        );
      }
      await this.fill();
    }
    const out = this.buffer.subarray(0, n);
    this.buffer = this.buffer.subarray(n);
    return out;
  }
}
