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

export interface DeviceStatDeck {
  slug: string;
  name: string;
  lang: string;
  cards: number;
  learning: number;
  due: number;
  fresh: number;
}

/**
 * Snapshot of device state, as built by main.cpp's on_stat_query(). `fw` and
 * `battery` are omitted by firmware that can't report them, and `time` is 0
 * when the device's clock has never been set (there's no RTC; see CLAUDE.md).
 */
export interface DeviceStat {
  fw?: string;
  battery?: { pct: number; charging: boolean };
  time: number;
  reviews_today: number;
  decks: DeviceStatDeck[];
}

function asRecord(value: unknown): Record<string, unknown> | null {
  if (typeof value !== "object" || value === null || Array.isArray(value)) return null;
  return value as Record<string, unknown>;
}

function num(value: unknown, fallback = 0): number {
  return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

function str(value: unknown, fallback = ""): string {
  return typeof value === "string" ? value : fallback;
}

/**
 * Parses the JSON body of an `@ok stat` reply. Deliberately lenient about
 * everything except "is this an object at all": firmware older or newer than
 * this client may omit fields or add ones we don't know about, and a
 * dashboard that shows a blank battery is far better than one that refuses to
 * load. Only unparseable JSON or a non-object root throws.
 */
export function parseDeviceStat(json: string): DeviceStat {
  let raw: unknown;
  try {
    raw = JSON.parse(json);
  } catch (err) {
    throw new DeviceError(`@stat: malformed JSON: ${(err as Error).message}`);
  }
  const root = asRecord(raw);
  if (!root) throw new DeviceError(`@stat: expected a JSON object, got ${JSON.stringify(json)}`);

  const stat: DeviceStat = {
    time: num(root.time),
    reviews_today: num(root.reviews_today),
    decks: [],
  };

  if (typeof root.fw === "string" && root.fw) stat.fw = root.fw;

  const battery = asRecord(root.battery);
  if (battery) {
    stat.battery = { pct: num(battery.pct), charging: battery.charging === true };
  }

  if (Array.isArray(root.decks)) {
    for (const entry of root.decks) {
      const deck = asRecord(entry);
      if (!deck) continue;
      stat.decks.push({
        slug: str(deck.slug),
        name: str(deck.name),
        lang: str(deck.lang),
        cards: num(deck.cards),
        learning: num(deck.learning),
        due: num(deck.due),
        fresh: num(deck.fresh),
      });
    }
  }

  return stat;
}

function crcHex(data: Uint8Array): string {
  return crc32(data).toString(16).padStart(8, "0");
}

const CHUNK_SIZE = 4096;

/**
 * Default time to wait for a device reply before giving up. Every
 * `@`-prefixed round trip goes through {@link DeviceClient.readControlLine}
 * or the chunked reads in fget/fput, both guarded by {@link withTimeout}, so
 * a lost or never-sent reply degrades into a rejected promise instead of an
 * unresolved one. Without this, a single stalled reply (e.g. the device
 * wedged mid `@fls`/`@fget`) hangs the caller forever with nothing to catch
 * and nothing to log — the bug this constant exists to prevent.
 */
const DEFAULT_TIMEOUT_MS = 10_000;

export interface DeviceClientOptions {
  /** Overridable for tests that want to hit the timeout quickly. */
  timeoutMs?: number;
}

export class DeviceClient {
  private readonly reader: LineReader;
  private readonly timeoutMs: number;

  constructor(
    private readonly transport: SerialTransport,
    opts: DeviceClientOptions = {},
  ) {
    this.reader = new LineReader(transport);
    this.timeoutMs = opts.timeoutMs ?? DEFAULT_TIMEOUT_MS;
  }

  /** Races `promise` against `this.timeoutMs`, rejecting with a DeviceError on expiry. */
  private async withTimeout<T>(context: string, promise: Promise<T>): Promise<T> {
    let timer: ReturnType<typeof setTimeout> | undefined;
    const timeout = new Promise<never>((_, reject) => {
      timer = setTimeout(() => {
        reject(new DeviceError(`${context}: no reply after ${this.timeoutMs}ms`));
      }, this.timeoutMs);
    });
    try {
      return await Promise.race([promise, timeout]);
    } finally {
      clearTimeout(timer);
    }
  }

  /** Reads lines until one starts with "@", discarding interleaved ESP_LOG output. */
  private async readControlLine(context = "reply"): Promise<string> {
    return this.withTimeout(
      context,
      (async () => {
        for (;;) {
          const line = await this.reader.readLine();
          if (line.startsWith("@")) return line;
        }
      })(),
    );
  }

  /** Reads exactly `n` bytes, timing out per chunk rather than for the whole transfer. */
  private async readExactWithTimeout(n: number, context: string): Promise<Uint8Array> {
    return this.withTimeout(context, this.reader.readExact(n));
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
    const line = await this.readControlLine("@ping");
    this.assertOk(line, "@ping");
  }

  /** Hands the device the host's clock; see CLAUDE.md, there's no RTC on this board. */
  async syncTime(unixSeconds: number): Promise<void> {
    await this.writeLine(`@time ${Math.floor(unixSeconds)}`);
    const line = await this.readControlLine("@time");
    this.assertOk(line, "@time");
  }

  /**
   * Device-state snapshot: battery, clock, today's review count, and per-deck
   * queue counts. Throws DeviceError on `@err`, which is what firmware
   * without an `@stat` handler replies, so callers can degrade gracefully.
   */
  async stat(): Promise<DeviceStat> {
    await this.writeLine("@stat");
    const line = await this.readControlLine("@stat");
    if (!line.startsWith("@ok stat ")) {
      this.assertOk(line, "@stat");
      throw new DeviceError(`@stat: unexpected reply ${JSON.stringify(line)}`);
    }
    return parseDeviceStat(line.slice("@ok stat ".length));
  }

  /**
   * Pushes `data` to `path` (relative to /data on the device). `onProgress`
   * is called after each chunk with (bytesSent, totalBytes).
   */
  async fput(path: string, data: Uint8Array, onProgress?: ProgressCallback): Promise<void> {
    await this.writeLine(`@fput ${path} ${data.length} ${crcHex(data)}`);

    const ready = await this.readControlLine("@fput");
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

    const done = await this.readControlLine("@fput");
    this.assertOk(done, "@fput");
  }

  /** Pulls `path` from the device, verifying the CRC-32 it reports. */
  async fget(path: string, onProgress?: ProgressCallback): Promise<Uint8Array> {
    await this.writeLine(`@fget ${path}`);

    const header = await this.readControlLine("@fget");
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
      const chunk = await this.readExactWithTimeout(want, "@fget");
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
    const line = await this.readControlLine("@fls");
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
    const line = await this.readControlLine("@fdel");
    this.assertOk(line, "@fdel");
  }

  async reboot(): Promise<void> {
    await this.writeLine("@reboot");
    const line = await this.readControlLine("@reboot");
    this.assertOk(line, "@reboot");
  }
}
