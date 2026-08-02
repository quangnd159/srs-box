// Module-level singleton owning the device connection. It exists outside
// React on purpose: DevicePage used to hold the transport/DeviceClient/log
// in useState, which React tears down on unmount. Navigating away from
// /device unmounted the component, the port was simply forgotten (never
// closed, just leaked out of state) and the granted port then looked
// "already open" to a fresh requestPort()+open() call, which WebSerial
// rejects with InvalidStateError. Routing everything through this store
// means the connection outlives the component; the page becomes a
// useSyncExternalStore view over it (see lib/deck/useDeckStore.ts for the
// SSR-safe mounted-gate pattern this follows).
//
// All device I/O is serialized through `run()`'s single queue. Before this,
// nothing prevented two protocol commands from being in flight on the same
// reply stream at once (e.g. an auto-reconnect racing a manual Connect
// click), and DeviceClient had no timeout, so a lost reply hung its caller
// forever with nothing to catch and nothing to log — busy stayed true and
// every button stayed disabled with no diagnostic. `run()` fixes the
// ordering; protocol.ts's per-command timeout (DeviceClient's
// DEFAULT_TIMEOUT_MS) fixes the hang; this store's catch-and-log in `run()`
// guarantees a stuck command always ends with an error line and `busy`
// cleared, never a silently-wedged UI.
import { buildDeck } from "@/lib/compiler/build";
import { parseDeck } from "@/lib/compiler/parse-srs";
import type { BuildResult } from "@/lib/compiler/types";
import { browserStore, DeckStore, type KeyValueStore } from "@/lib/deck/store";
import type { Card, Deck } from "@/lib/deck/types";
import type { CardInfo } from "@/lib/revlog/ankiExport";
import { parseRevlog } from "@/lib/revlog/parse";
import { saveRevlog } from "@/app/revlogStorage";
import { DeviceClient, type DeviceStat } from "./protocol";
import type { SerialTransport } from "./transport";
import { WebSerialTransport } from "./webSerialTransport";

export interface LogEntry {
  text: string;
  kind: "info" | "error" | "progress";
}

/** One `decks/<slug>.srs` found on the device, parsed where possible. */
export interface DeviceDeck {
  path: string;
  slug: string;
  size: number;
  name: string;
  lang: string;
  cards: number;
  /** Header CRC-32, the whole point of the sync comparison below. */
  crc32: number;
  error?: string;
}

export interface CompiledLocal {
  deck: Deck;
  result: BuildResult;
}

export type SyncStatus = "in sync" | "outdated" | "not local" | "not on device" | "unknown";

export interface DeckRow {
  slug: string;
  name: string;
  lang: string;
  status: SyncStatus;
  device?: DeviceDeck;
  local?: CompiledLocal;
  localError?: string;
  counts?: { due: number; learning: number; fresh: number };
}

/**
 * Compares a device deck against the local one by header CRC-32. That CRC
 * covers the whole payload, so any edit that would change what the device
 * shows changes it, and the local compiler is byte-identical to the one that
 * produced the file on the device. Two cases can't be answered: a deck file
 * the device can't parse, and a local deck that doesn't compile; both report
 * "unknown" rather than guessing "outdated".
 */
export function syncStatus(device: DeviceDeck, local: CompiledLocal | string | undefined): SyncStatus {
  if (local === undefined) return "not local";
  if (typeof local === "string" || device.error) return "unknown";
  return parseDeck(local.result.bytes).header.crc32 === device.crc32 ? "in sync" : "outdated";
}

export type ConnectionStatus = "disconnected" | "connecting" | "connected";

export interface ConnectionSnapshot {
  status: ConnectionStatus;
  busy: boolean;
  log: LogEntry[];
  stat: DeviceStat | null;
  /** Host clock (unix seconds) when `stat` was fetched; see formatDrift() in the page. */
  statAt: number;
  rows: DeckRow[] | null;
  deviceCards: Map<bigint, CardInfo>;
  pullSummary: string | null;
}

const EMPTY_SNAPSHOT: ConnectionSnapshot = {
  status: "disconnected",
  busy: false,
  log: [],
  stat: null,
  statAt: 0,
  rows: null,
  deviceCards: new Map(),
  pullSummary: null,
};

/** What the store needs from a transport: the protocol duplex, plus lifecycle. */
export interface TransportHandle extends SerialTransport {
  close(): Promise<void>;
  onDisconnect(cb: () => void): () => void;
}

/** How the store obtains transports; swapped for a fake in tests. */
export interface ConnectionEnvironment {
  /** Opens a brand-new picker-granted port; only valid from a user gesture. */
  requestAndOpen(): Promise<TransportHandle>;
  /** Reattaches to a previously-granted port with no picker; null if none/several match. */
  autoConnect(): Promise<TransportHandle | null>;
}

/** The subset of DeckStore that push/refresh need, so tests can supply a fake. */
export interface DeckSource {
  listDecks(): Deck[];
  getCards(slug: string): Card[];
}

function defaultEnvironment(): ConnectionEnvironment {
  return {
    requestAndOpen: () => WebSerialTransport.requestAndOpen(),
    autoConnect: () => WebSerialTransport.autoConnect(),
  };
}

export interface ConnectionStoreOptions {
  env?: ConnectionEnvironment;
  deckSource?: () => DeckSource;
  kv?: () => KeyValueStore;
  /** Per-command reply timeout handed to DeviceClient; shortened in tests. */
  timeoutMs?: number;
}

export class ConnectionStore {
  private readonly env: ConnectionEnvironment;
  private readonly deckSource: () => DeckSource;
  private readonly kv: () => KeyValueStore;
  private readonly timeoutMs: number | undefined;

  private status: ConnectionStatus = "disconnected";
  private busy = false;
  private log: LogEntry[] = [];
  private stat: DeviceStat | null = null;
  private statAt = 0;
  private rows: DeckRow[] | null = null;
  private deviceCards: Map<bigint, CardInfo> = new Map();
  private pullSummary: string | null = null;

  private transport: TransportHandle | null = null;
  private client: DeviceClient | null = null;
  private detachDisconnect: (() => void) | null = null;

  /** Single-file queue: every device operation chains onto this so two can
   * never race reads on the one reply stream (see file header). */
  private queue: Promise<void> = Promise.resolve();

  private snapshot: ConnectionSnapshot = EMPTY_SNAPSHOT;
  private readonly listeners = new Set<() => void>();

  constructor(opts: ConnectionStoreOptions = {}) {
    this.env = opts.env ?? defaultEnvironment();
    this.deckSource = opts.deckSource ?? (() => new DeckStore(browserStore()));
    this.kv = opts.kv ?? browserStore;
    this.timeoutMs = opts.timeoutMs;
  }

  subscribe = (cb: () => void): (() => void) => {
    this.listeners.add(cb);
    return () => this.listeners.delete(cb);
  };

  getSnapshot = (): ConnectionSnapshot => this.snapshot;

  getServerSnapshot = (): ConnectionSnapshot => EMPTY_SNAPSHOT;

  private notify(): void {
    this.snapshot = {
      status: this.status,
      busy: this.busy,
      log: this.log,
      stat: this.stat,
      statAt: this.statAt,
      rows: this.rows,
      deviceCards: this.deviceCards,
      pullSummary: this.pullSummary,
    };
    for (const l of this.listeners) l();
  }

  private pushLog(text: string, kind: LogEntry["kind"] = "info"): void {
    this.log = [...this.log, { text, kind }];
  }

  /**
   * Runs `fn` after every previously-queued operation has settled, with
   * `busy` true for its whole run. Any throw (including a DeviceClient
   * timeout) is caught, logged as an error line, and `busy` is always
   * cleared in `finally` — a stuck command degrades into a visible error,
   * never a permanently-disabled UI.
   */
  private run(label: string, fn: () => Promise<void>): Promise<void> {
    const task = this.queue.then(async () => {
      this.busy = true;
      this.notify();
      try {
        await fn();
      } catch (err) {
        this.pushLog(`${label} failed: ${(err as Error).message}`, "error");
      } finally {
        this.busy = false;
        this.notify();
      }
    });
    this.queue = task;
    return task;
  }

  private async teardown(): Promise<void> {
    this.detachDisconnect?.();
    this.detachDisconnect = null;
    await this.transport?.close().catch(() => {});
    this.transport = null;
    this.client = null;
    this.status = "disconnected";
    this.stat = null;
    this.statAt = 0;
    this.rows = null;
    this.deviceCards = new Map();
  }

  private onDeviceDisconnected(): void {
    void this.run("device disconnected", async () => {
      if (!this.transport) return; // already torn down (e.g. by an explicit disconnect)
      this.pushLog("device disconnected", "error");
      await this.teardown();
    });
  }

  private async doConnect(getTransport: () => Promise<TransportHandle | null>): Promise<void> {
    if (this.status !== "disconnected") return;
    this.status = "connecting";
    this.notify();

    let transport: TransportHandle | null;
    try {
      transport = await getTransport();
    } catch (err) {
      this.status = "disconnected";
      throw err;
    }
    if (!transport) {
      this.status = "disconnected";
      return;
    }

    this.transport = transport;
    this.detachDisconnect = transport.onDisconnect(() => this.onDeviceDisconnected());
    this.client = new DeviceClient(
      transport,
      this.timeoutMs !== undefined ? { timeoutMs: this.timeoutMs } : {},
    );
    this.status = "connected";
    this.pushLog("connected");
    this.notify();

    try {
      await this.client.syncTime(Math.floor(Date.now() / 1000));
      this.pushLog("clock synced");
    } catch (err) {
      this.pushLog(`auto clock sync failed: ${(err as Error).message}`, "error");
    }
    await this.refreshLocked();
    this.pushLog("device state read");
  }

  /** Connect button: opens a fresh picker-granted port. */
  connect(): Promise<void> {
    return this.run("connect", () => this.doConnect(() => this.env.requestAndOpen()));
  }

  /**
   * Called once on /device mount. Silently does nothing unless exactly one
   * previously-granted port matches the device's vendor id (see
   * WebSerialTransport.autoConnect) and the store is currently disconnected,
   * so it never fights a connection already in progress or steals the
   * picker's job when there's ambiguity.
   */
  tryAutoReconnect(): Promise<void> {
    if (this.status !== "disconnected" || this.busy) return Promise.resolve();
    return this.run("auto-reconnect", () => this.doConnect(() => this.env.autoConnect()));
  }

  disconnect(): Promise<void> {
    return this.run("disconnect", async () => {
      await this.teardown();
      this.pushLog("disconnected");
    });
  }

  refresh(): Promise<void> {
    return this.run("refresh", async () => {
      await this.refreshLocked();
      this.pushLog("device state read");
    });
  }

  syncTime(): Promise<void> {
    return this.run("@time", async () => {
      if (!this.client) return;
      await this.client.syncTime(Math.floor(Date.now() / 1000));
      this.pushLog("clock synced");
      await this.refreshLocked();
    });
  }

  /** Compiles every local deck, so its bytes can be compared or pushed. */
  private async compileLocals(): Promise<Map<string, CompiledLocal | string>> {
    const source = this.deckSource();
    const out = new Map<string, CompiledLocal | string>();
    for (const deck of source.listDecks()) {
      const cards = source.getCards(deck.slug).filter((c) => c.front.trim());
      try {
        const result = await buildDeck(cards, { name: deck.name, slug: deck.slug, lang: deck.lang });
        out.set(deck.slug, { deck, result });
      } catch (err) {
        out.set(deck.slug, (err as Error).message);
      }
    }
    return out;
  }

  /**
   * Reads the whole device state: @stat (optional), the deck files, and the
   * local decks compiled for comparison. Also rebuilds the card_id -> deck
   * map a later revlog pull needs to attribute reviews. Callers run this
   * inside `run()`, so a throw here is caught, logged, and clears `busy`
   * rather than hanging.
   */
  private async refreshLocked(): Promise<void> {
    const c = this.client;
    if (!c) return;

    let statResult: DeviceStat | null = null;
    try {
      statResult = await c.stat();
      this.stat = statResult;
      this.statAt = Math.floor(Date.now() / 1000);
    } catch (err) {
      // Firmware without an @stat handler replies @err; everything else on
      // this page works without it, so this is a downgrade, not a failure.
      this.stat = null;
      this.pushLog(`@stat unavailable (${(err as Error).message}); summary hidden`, "progress");
    }
    this.notify();

    const listing = await c.fls();
    if (listing.truncated) this.pushLog("@fls reply was truncated; some files may be missing", "progress");

    const deviceDecks: DeviceDeck[] = [];
    const cardMap = new Map<bigint, CardInfo>();
    for (const entry of listing.entries) {
      if (!entry.path.startsWith("decks/") || !entry.path.endsWith(".srs")) continue;
      const fallbackSlug = entry.path.slice("decks/".length, -".srs".length);
      try {
        const data = await c.fget(entry.path);
        const parsed = parseDeck(data);
        const slug = parsed.meta.slug || fallbackSlug;
        deviceDecks.push({
          path: entry.path,
          slug,
          size: entry.size,
          name: parsed.meta.name || slug,
          lang: parsed.meta.lang || "",
          cards: parsed.cards.length,
          crc32: parsed.header.crc32,
        });
        for (const card of parsed.cards) cardMap.set(card.id, { deckSlug: slug, front: card.front });
      } catch (err) {
        deviceDecks.push({
          path: entry.path,
          slug: fallbackSlug,
          size: entry.size,
          name: fallbackSlug,
          lang: "",
          cards: 0,
          crc32: 0,
          error: (err as Error).message,
        });
      }
    }
    this.deviceCards = cardMap;

    const locals = await this.compileLocals();
    const statBySlug = new Map(statResult?.decks.map((d) => [d.slug, d]) ?? []);

    const merged: DeckRow[] = [];
    for (const device of deviceDecks) {
      const local = locals.get(device.slug);
      const counts = statBySlug.get(device.slug);
      const compiled = typeof local === "object" ? local : undefined;
      merged.push({
        slug: device.slug,
        name: device.name,
        lang: device.lang,
        device,
        local: compiled,
        localError: typeof local === "string" ? local : undefined,
        status: syncStatus(device, local),
        counts: counts ? { due: counts.due, learning: counts.learning, fresh: counts.fresh } : undefined,
      });
    }
    for (const [slug, local] of locals) {
      if (deviceDecks.some((d) => d.slug === slug)) continue;
      const compiled = typeof local === "object" ? local : undefined;
      merged.push({
        slug,
        name: compiled?.deck.name ?? slug,
        lang: compiled?.deck.lang ?? "",
        local: compiled,
        localError: typeof local === "string" ? local : undefined,
        status: "not on device",
      });
    }
    this.rows = merged;
  }

  /**
   * Fonts are global: one subset over the union of every local deck's
   * glyphs (docs/sync-protocol.md), so pushing a single deck still has to
   * re-push them or a new hanzi renders as a blank box.
   */
  private async pushFonts(c: DeviceClient, locals: Map<string, CompiledLocal | string>): Promise<void> {
    const glyphSets = [...locals.values()]
      .filter((l): l is CompiledLocal => typeof l === "object")
      .map((l) => l.result.glyphs.join(""));
    if (glyphSets.length === 0) return;

    this.pushLog("subsetting fonts over the union of all decks' glyphs...");
    const res = await fetch("/api/font", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ deckGlyphSets: glyphSets }),
    });
    if (!res.ok) {
      const body = (await res.json().catch(() => ({ error: res.statusText }))) as { error?: string };
      throw new Error(body.error ?? res.statusText);
    }
    const { fonts } = (await res.json()) as { fonts: Record<string, string> };

    for (const [size, base64] of Object.entries(fonts)) {
      const data = Uint8Array.from(atob(base64), (ch) => ch.charCodeAt(0));
      this.pushLog(`pushing fonts/font_cjk_${size}.bin (${data.length.toLocaleString()} bytes)`);
      await c.fput(`fonts/font_cjk_${size}.bin`, data, (sent, total) => {
        this.pushLog(`  ${sent}/${total} bytes`, "progress");
      });
      this.pushLog(`fonts/font_cjk_${size}.bin ok`);
    }
  }

  pushDeck(slug: string): Promise<void> {
    return this.run(`push ${slug}`, async () => {
      const c = this.client;
      if (!c) return;
      const locals = await this.compileLocals();
      const local = locals.get(slug);
      if (!local) throw new Error(`no local deck "${slug}"`);
      if (typeof local === "string") throw new Error(local);

      this.pushLog(`pushing decks/${slug}.srs (${local.result.bytes.length.toLocaleString()} bytes)`);
      await c.fput(`decks/${slug}.srs`, local.result.bytes, (sent, total) => {
        this.pushLog(`  ${sent}/${total} bytes`, "progress");
      });
      this.pushLog(`decks/${slug}.srs ok`);

      await this.pushFonts(c, locals);
      await c.reboot();
      this.pushLog("rebooting device to apply pushed content");
      await this.teardown();
      this.pushLog("disconnected");
    });
  }

  pushAll(): Promise<void> {
    return this.run("push all", async () => {
      const c = this.client;
      if (!c) return;
      const locals = await this.compileLocals();
      const compiled = [...locals.values()].filter((l): l is CompiledLocal => typeof l === "object");
      if (compiled.length === 0) throw new Error("no compilable local decks");

      for (const local of compiled) {
        this.pushLog(`pushing decks/${local.deck.slug}.srs (${local.result.bytes.length.toLocaleString()} bytes)`);
        await c.fput(`decks/${local.deck.slug}.srs`, local.result.bytes, (sent, total) => {
          this.pushLog(`  ${sent}/${total} bytes`, "progress");
        });
        this.pushLog(`decks/${local.deck.slug}.srs ok`);
      }

      await this.pushFonts(c, locals);
      await c.reboot();
      this.pushLog("rebooting device to apply pushed content");
      await this.teardown();
      this.pushLog("disconnected");
    });
  }

  deleteDeck(path: string): Promise<void> {
    return this.run(`@fdel ${path}`, async () => {
      const c = this.client;
      if (!c) return;
      await c.fdel(path);
      this.pushLog(`${path} deleted`);
      await this.refreshLocked();
    });
  }

  pullRevlog(): Promise<void> {
    return this.run("pull", async () => {
      const c = this.client;
      if (!c) return;
      this.pushLog("pulling revlog.bin");
      const data = await c.fget("revlog.bin", (received, total) => {
        this.pushLog(`  ${received}/${total} bytes`, "progress");
      });

      this.triggerDownload(data, "revlog.bin");
      this.pushLog(`revlog.bin ok, ${data.length.toLocaleString()} bytes, crc verified`);

      const parsed = parseRevlog(data);
      saveRevlog(this.kv(), {
        pulledAt: new Date().toISOString(),
        version: parsed.version,
        entries: parsed.entries,
        cards: this.deviceCards,
      });
      if (parsed.truncatedTail) this.pushLog("revlog ended mid-record; trailing bytes ignored", "progress");

      if (parsed.entries.length === 0) {
        this.pullSummary = "No reviews in the log yet.";
        return;
      }
      const times = parsed.entries.map((e) => Number(e.reviewed) * 1000);
      const first = new Date(Math.min(...times)).toISOString().slice(0, 10);
      const last = new Date(Math.max(...times)).toISOString().slice(0, 10);
      this.pullSummary = `${parsed.entries.length.toLocaleString()} reviews, ${first} to ${last}. Saved for /stats.`;
    });
  }

  private triggerDownload(data: Uint8Array, filename: string): void {
    if (typeof document === "undefined") return;
    const blob = new Blob([data.buffer as ArrayBuffer], { type: "application/octet-stream" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
  }
}

/** The one connection this tab has to the device; see the file header. */
export const connectionStore = new ConnectionStore();
