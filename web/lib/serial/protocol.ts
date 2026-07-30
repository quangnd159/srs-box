// Sync protocol client, transport-agnostic (see transport.ts) so it can be
// unit tested over a mock duplex stream without WebSerial or real hardware.
// Implements docs/sync-protocol.md: @ping, @time, @fput, @fget, @fls,
// @fdel, @reboot, all riding the same `@`-prefixed CDC link as @shot/@tap
// (tools/devctl.py, firmware/components/devctl/).
import { crc32 } from "../compiler/crc32";
import { LineReader } from "./lineReader";
import type { SerialTransport } from "./transport";

export class DeviceError extends Error {}

export interface FlsEntry {
  path: string;
  size: number;
}

export interface FlsResult {
  entries: FlsEntry[];
  truncated: boolean;
}

export type ProgressCallback = (sent: number, total: number) => void;

function crcHex(data: Uint8Array): string {
  return crc32(data).toString(16).padStart(8, "0");
}

const CHUNK_SIZE = 4096;

export class DeviceClient {
  private readonly reader: LineReader;

  constructor(private readonly transport: SerialTransport) {
    this.reader = new LineReader(transport);
  }

  /** Reads lines until one starts with "@", discarding interleaved ESP_LOG output. */
  private async readControlLine(): Promise<string> {
    for (;;) {
      const line = await this.reader.readLine();
      if (line.startsWith("@")) return line;
    }
  }

  private async writeLine(line: string): Promise<void> {
    await this.transport.write(new TextEncoder().encode(line + "\n"));
  }

  private assertOk(line: string, context: string): void {
    if (line.startsWith("@err")) throw new DeviceError(`${context}: ${line}`);
    if (!line.startsWith("@ok")) {
      throw new DeviceError(`${context}: unexpected reply ${JSON.stringify(line)}`);
    }
  }

  async ping(): Promise<void> {
    await this.writeLine("@ping");
    const line = await this.readControlLine();
    this.assertOk(line, "@ping");
  }

  /** Hands the device the host's clock; see CLAUDE.md, there's no RTC on this board. */
  async syncTime(unixSeconds: number): Promise<void> {
    await this.writeLine(`@time ${Math.floor(unixSeconds)}`);
    const line = await this.readControlLine();
    this.assertOk(line, "@time");
  }

  /**
   * Pushes `data` to `path` (relative to /data on the device). `onProgress`
   * is called after each chunk with (bytesSent, totalBytes).
   */
  async fput(path: string, data: Uint8Array, onProgress?: ProgressCallback): Promise<void> {
    await this.writeLine(`@fput ${path} ${data.length} ${crcHex(data)}`);

    const ready = await this.readControlLine();
    if (ready !== "@ok send") {
      this.assertOk(ready, "@fput");
      throw new DeviceError(`@fput: expected "@ok send", got ${JSON.stringify(ready)}`);
    }

    let sent = 0;
    while (sent < data.length) {
      const end = Math.min(sent + CHUNK_SIZE, data.length);
      await this.transport.write(data.subarray(sent, end));
      sent = end;
      onProgress?.(sent, data.length);
    }

    const done = await this.readControlLine();
    this.assertOk(done, "@fput");
  }

  /** Pulls `path` from the device, verifying the CRC-32 it reports. */
  async fget(path: string, onProgress?: ProgressCallback): Promise<Uint8Array> {
    await this.writeLine(`@fget ${path}`);

    const header = await this.readControlLine();
    if (!header.startsWith("@fget ")) {
      this.assertOk(header, "@fget");
      throw new DeviceError(`@fget: expected a file header, got ${JSON.stringify(header)}`);
    }
    const parts = header.split(" ");
    const nbytes = Number(parts[1]);
    const crcHexExpected = parts[2];
    if (!Number.isFinite(nbytes) || !crcHexExpected) {
      throw new DeviceError(`@fget: malformed header ${JSON.stringify(header)}`);
    }

    const chunks: Uint8Array[] = [];
    let received = 0;
    while (received < nbytes) {
      const want = Math.min(CHUNK_SIZE, nbytes - received);
      const chunk = await this.reader.readExact(want);
      chunks.push(chunk.slice());
      received += chunk.length;
      onProgress?.(received, nbytes);
    }

    const data = new Uint8Array(nbytes);
    let offset = 0;
    for (const c of chunks) {
      data.set(c, offset);
      offset += c.length;
    }

    const actual = crcHex(data);
    if (actual !== crcHexExpected) {
      throw new DeviceError(`@fget: crc mismatch, device said ${crcHexExpected}, got ${actual}`);
    }
    return data;
  }

  async fls(): Promise<FlsResult> {
    await this.writeLine("@fls");
    const line = await this.readControlLine();
    if (!line.startsWith("@ok fls")) {
      this.assertOk(line, "@fls");
      throw new DeviceError(`@fls: unexpected reply ${JSON.stringify(line)}`);
    }
    const rest = line.slice("@ok fls".length).trim();
    if (!rest) return { entries: [], truncated: false };

    const tokens = rest.split(/\s+/);
    let truncated = false;
    const entries: FlsEntry[] = [];
    for (const tok of tokens) {
      if (tok === "...") {
        truncated = true;
        continue;
      }
      const idx = tok.lastIndexOf("=");
      if (idx === -1) continue;
      entries.push({ path: tok.slice(0, idx), size: Number(tok.slice(idx + 1)) });
    }
    return { entries, truncated };
  }

  async fdel(path: string): Promise<void> {
    await this.writeLine(`@fdel ${path}`);
    const line = await this.readControlLine();
    this.assertOk(line, "@fdel");
  }

  async reboot(): Promise<void> {
    await this.writeLine("@reboot");
    const line = await this.readControlLine();
    this.assertOk(line, "@reboot");
  }
}
