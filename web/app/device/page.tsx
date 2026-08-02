"use client";

import Link from "next/link";
import { useState } from "react";
import { buildDeck } from "@/lib/compiler/build";
import { parseDeck } from "@/lib/compiler/parse-srs";
import type { BuildResult } from "@/lib/compiler/types";
import { browserStore } from "@/lib/deck/store";
import type { Deck } from "@/lib/deck/types";
import { useDeckStore, useHasMounted } from "@/lib/deck/useDeckStore";
import type { CardInfo } from "@/lib/revlog/ankiExport";
import { parseRevlog } from "@/lib/revlog/parse";
import { DeviceClient, type DeviceStat } from "@/lib/serial/protocol";
import { WebSerialTransport } from "@/lib/serial/webSerialTransport";
import { saveRevlog } from "../revlogStorage";

interface LogEntry {
  text: string;
  kind: "info" | "error" | "progress";
}

/** One `decks/<slug>.srs` found on the device, parsed where possible. */
interface DeviceDeck {
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

interface CompiledLocal {
  deck: Deck;
  result: BuildResult;
}

type SyncStatus = "in sync" | "outdated" | "not local" | "not on device" | "unknown";

interface DeckRow {
  slug: string;
  name: string;
  lang: string;
  status: SyncStatus;
  device?: DeviceDeck;
  local?: CompiledLocal;
  localError?: string;
  counts?: { due: number; learning: number; fresh: number };
}

const STATUS_CLASS: Record<SyncStatus, string> = {
  "in sync": "text-emerald-600 dark:text-emerald-400",
  outdated: "text-amber-600 dark:text-amber-400",
  "not local": "text-zinc-500",
  "not on device": "text-sky-600 dark:text-sky-400",
  unknown: "text-zinc-500",
};

function formatDrift(seconds: number): string {
  const abs = Math.abs(seconds);
  if (abs < 2) return "in step with this computer";
  const magnitude =
    abs < 90 ? `${abs}s` : abs < 5400 ? `${Math.round(abs / 60)}m` : `${Math.round(abs / 3600)}h`;
  return `${magnitude} ${seconds > 0 ? "ahead of" : "behind"} this computer`;
}

/**
 * Compares a device deck against the local one by header CRC-32. That CRC
 * covers the whole payload — META (name, slug, lang, card count), the card
 * table, and the text pool — so any edit that would change what the device
 * shows changes it, and the local compiler is byte-identical to the one that
 * produced the file on the device. Two cases can't be answered: a deck file
 * the device can't parse, and a local deck that doesn't compile; both report
 * "unknown" rather than guessing "outdated".
 */
function syncStatus(device: DeviceDeck, local: CompiledLocal | string | undefined): SyncStatus {
  if (local === undefined) return "not local";
  // A string here is a compile error message; see compileLocals().
  if (typeof local === "string" || device.error) return "unknown";
  return parseDeck(local.result.bytes).header.crc32 === device.crc32 ? "in sync" : "outdated";
}

function bytes(n: number): string {
  return n < 1024 ? `${n} B` : `${(n / 1024).toFixed(1)} KB`;
}

export default function DevicePage() {
  const hasMounted = useHasMounted();
  const store = useDeckStore();
  const [client, setClient] = useState<DeviceClient | null>(null);
  const [transport, setTransport] = useState<WebSerialTransport | null>(null);
  const [log, setLog] = useState<LogEntry[]>([]);
  const [busy, setBusy] = useState(false);

  const [stat, setStat] = useState<DeviceStat | null>(null);
  // Host clock at the moment @stat replied, so the drift readout stays honest
  // even if the page then sits open for an hour.
  const [statAt, setStatAt] = useState(0);
  const [rows, setRows] = useState<DeckRow[] | null>(null);
  const [pullSummary, setPullSummary] = useState<string | null>(null);
  // Card ids of everything currently on the device, so a revlog pull can be
  // attributed to decks and headwords without a second round trip.
  const [deviceCards, setDeviceCards] = useState<Map<bigint, CardInfo>>(new Map());

  const serialSupported =
    !hasMounted || typeof navigator === "undefined"
      ? true
      : "serial" in navigator;

  function push(text: string, kind: LogEntry["kind"] = "info") {
    setLog((l) => [...l, { text, kind }]);
  }

  /** Compiles every local deck, so its bytes can be compared or pushed. */
  async function compileLocals(): Promise<Map<string, CompiledLocal | string>> {
    const out = new Map<string, CompiledLocal | string>();
    for (const deck of store.listDecks()) {
      const cards = store.getCards(deck.slug).filter((c) => c.front.trim());
      try {
        const result = await buildDeck(cards, {
          name: deck.name,
          slug: deck.slug,
          lang: deck.lang,
        });
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
   * map that a later revlog pull needs to attribute reviews.
   */
  async function refresh(c: DeviceClient): Promise<void> {
    let statResult: DeviceStat | null = null;
    try {
      statResult = await c.stat();
      setStat(statResult);
      setStatAt(Math.floor(Date.now() / 1000));
    } catch (err) {
      // Firmware without an @stat handler replies @err; everything else on
      // this page works without it, so this is a downgrade, not a failure.
      setStat(null);
      push(`@stat unavailable (${(err as Error).message}); summary hidden`, "progress");
    }

    const listing = await c.fls();
    if (listing.truncated) push("@fls reply was truncated; some files may be missing", "progress");

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
    setDeviceCards(cardMap);

    const locals = await compileLocals();
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
        counts: counts
          ? { due: counts.due, learning: counts.learning, fresh: counts.fresh }
          : undefined,
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
    setRows(merged);
  }

  async function withDevice(label: string, fn: (c: DeviceClient) => Promise<void>): Promise<void> {
    if (!client || busy) return;
    setBusy(true);
    try {
      await fn(client);
    } catch (err) {
      push(`${label} failed: ${(err as Error).message}`, "error");
    } finally {
      setBusy(false);
    }
  }

  async function handleConnect() {
    setBusy(true);
    try {
      const t = await WebSerialTransport.requestAndOpen();
      const c = new DeviceClient(t);
      setTransport(t);
      setClient(c);
      push("connected");
      try {
        await c.syncTime(Math.floor(Date.now() / 1000));
        push("clock synced");
      } catch (err) {
        push(`auto clock sync failed: ${(err as Error).message}`, "error");
      }
      await refresh(c);
      push("device state read");
    } catch (err) {
      push(`connect failed: ${(err as Error).message}`, "error");
    } finally {
      setBusy(false);
    }
  }

  async function handleDisconnect() {
    await transport?.close();
    setTransport(null);
    setClient(null);
    setStat(null);
    setRows(null);
    setDeviceCards(new Map());
    push("disconnected");
  }

  function handleRefresh() {
    void withDevice("refresh", async (c) => {
      await refresh(c);
      push("device state read");
    });
  }

  function handleSyncTime() {
    void withDevice("@time", async (c) => {
      await c.syncTime(Math.floor(Date.now() / 1000));
      push("clock synced");
      await refresh(c);
    });
  }

  /**
   * Fonts are global: one subset over the union of every local deck's glyphs
   * (docs/sync-protocol.md), so pushing a single deck still has to re-push
   * them or a new hanzi renders as a blank box.
   */
  async function pushFonts(c: DeviceClient, locals: Map<string, CompiledLocal | string>) {
    const glyphSets = [...locals.values()]
      .filter((l): l is CompiledLocal => typeof l === "object")
      .map((l) => l.result.glyphs.join(""));
    if (glyphSets.length === 0) return;

    push("subsetting fonts over the union of all decks' glyphs...");
    const res = await fetch("/api/font", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ deckGlyphSets: glyphSets }),
    });
    if (!res.ok) {
      const body = (await res.json().catch(() => ({ error: res.statusText }))) as {
        error?: string;
      };
      throw new Error(body.error ?? res.statusText);
    }
    const { fonts } = (await res.json()) as { fonts: Record<string, string> };

    for (const [size, base64] of Object.entries(fonts)) {
      const data = Uint8Array.from(atob(base64), (ch) => ch.charCodeAt(0));
      push(`pushing fonts/font_cjk_${size}.bin (${data.length.toLocaleString()} bytes)`);
      await c.fput(`fonts/font_cjk_${size}.bin`, data, (sent, total) => {
        push(`  ${sent}/${total} bytes`, "progress");
      });
      push(`fonts/font_cjk_${size}.bin ok`);
    }
  }

  function handlePushDeck(slug: string) {
    if (!confirm(`Push "${slug}" and rebuilt fonts, then reboot the device?`)) return;
    void withDevice(`push ${slug}`, async (c) => {
      const locals = await compileLocals();
      const local = locals.get(slug);
      if (!local) throw new Error(`no local deck "${slug}"`);
      if (typeof local === "string") throw new Error(local);

      push(`pushing decks/${slug}.srs (${local.result.bytes.length.toLocaleString()} bytes)`);
      await c.fput(`decks/${slug}.srs`, local.result.bytes, (sent, total) => {
        push(`  ${sent}/${total} bytes`, "progress");
      });
      push(`decks/${slug}.srs ok`);

      await pushFonts(c, locals);
      await c.reboot();
      push("rebooting device to apply pushed content");
      await handleDisconnect();
    });
  }

  function handlePushAll() {
    if (!confirm("Push every local deck and rebuilt fonts, then reboot the device?")) return;
    void withDevice("push all", async (c) => {
      const locals = await compileLocals();
      const compiled = [...locals.values()].filter((l): l is CompiledLocal => typeof l === "object");
      if (compiled.length === 0) throw new Error("no compilable local decks");

      for (const local of compiled) {
        push(
          `pushing decks/${local.deck.slug}.srs (${local.result.bytes.length.toLocaleString()} bytes)`,
        );
        await c.fput(`decks/${local.deck.slug}.srs`, local.result.bytes, (sent, total) => {
          push(`  ${sent}/${total} bytes`, "progress");
        });
        push(`decks/${local.deck.slug}.srs ok`);
      }

      await pushFonts(c, locals);
      await c.reboot();
      push("rebooting device to apply pushed content");
      await handleDisconnect();
    });
  }

  function handleDeleteDeck(path: string) {
    if (!confirm(`Delete ${path} from the device? Its review history is kept in revlog.bin.`)) {
      return;
    }
    void withDevice(`@fdel ${path}`, async (c) => {
      await c.fdel(path);
      push(`${path} deleted`);
      await refresh(c);
    });
  }

  function handlePullRevlog() {
    void withDevice("pull", async (c) => {
      push("pulling revlog.bin");
      const data = await c.fget("revlog.bin", (received, total) => {
        push(`  ${received}/${total} bytes`, "progress");
      });

      const blob = new Blob([data.buffer as ArrayBuffer], { type: "application/octet-stream" });
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = "revlog.bin";
      a.click();
      URL.revokeObjectURL(url);
      push(`revlog.bin ok, ${data.length.toLocaleString()} bytes, crc verified`);

      const parsed = parseRevlog(data);
      saveRevlog(browserStore(), {
        pulledAt: new Date().toISOString(),
        version: parsed.version,
        entries: parsed.entries,
        cards: deviceCards,
      });
      if (parsed.truncatedTail) push("revlog ended mid-record; trailing bytes ignored", "progress");

      if (parsed.entries.length === 0) {
        setPullSummary("No reviews in the log yet.");
        return;
      }
      const times = parsed.entries.map((e) => Number(e.reviewed) * 1000);
      const first = new Date(Math.min(...times)).toISOString().slice(0, 10);
      const last = new Date(Math.max(...times)).toISOString().slice(0, 10);
      setPullSummary(
        `${parsed.entries.length.toLocaleString()} reviews, ${first} to ${last}. Saved for /stats.`,
      );
    });
  }

  return (
    <div className="mx-auto max-w-4xl px-6 py-10">
      <h1 className="text-2xl font-semibold">Device</h1>
      <p className="mt-1 text-sm text-zinc-600 dark:text-zinc-400">
        Connects over WebSerial, matching docs/sync-protocol.md.
      </p>

      {!serialSupported && (
        <p className="mt-4 rounded-lg border border-amber-300 p-3 text-sm text-amber-700 dark:border-amber-900 dark:text-amber-400">
          This browser has no Web Serial API. Use Chrome, Edge, or another
          Chromium-based browser to talk to the device.
        </p>
      )}

      <div className="mt-6 flex flex-wrap gap-2">
        {!client ? (
          <button
            onClick={handleConnect}
            disabled={busy}
            className="rounded bg-zinc-900 px-4 py-1.5 text-sm font-medium text-white disabled:opacity-40 dark:bg-zinc-100 dark:text-zinc-900"
          >
            Connect
          </button>
        ) : (
          <button
            onClick={handleDisconnect}
            className="rounded border border-zinc-300 px-4 py-1.5 text-sm dark:border-zinc-700"
          >
            Disconnect
          </button>
        )}
        <button
          onClick={handleRefresh}
          disabled={!client || busy}
          className="rounded border border-zinc-300 px-4 py-1.5 text-sm disabled:opacity-40 dark:border-zinc-700"
        >
          Refresh
        </button>
        <button
          onClick={handleSyncTime}
          disabled={!client || busy}
          className="rounded border border-zinc-300 px-4 py-1.5 text-sm disabled:opacity-40 dark:border-zinc-700"
        >
          Sync clock
        </button>
        <button
          onClick={handlePushAll}
          disabled={!client || busy}
          className="rounded bg-zinc-900 px-4 py-1.5 text-sm font-medium text-white disabled:opacity-40 dark:bg-zinc-100 dark:text-zinc-900"
        >
          Push everything
        </button>
        <button
          onClick={handlePullRevlog}
          disabled={!client || busy}
          className="rounded border border-zinc-300 px-4 py-1.5 text-sm disabled:opacity-40 dark:border-zinc-700"
        >
          Pull review log
        </button>
      </div>

      {pullSummary && (
        <p className="mt-3 text-sm text-zinc-600 dark:text-zinc-400">
          {pullSummary}{" "}
          <Link href="/stats" className="underline">
            Open stats
          </Link>
        </p>
      )}

      {stat && (
        <section className="mt-6 grid grid-cols-2 gap-4 rounded-lg border border-zinc-200 p-4 sm:grid-cols-4 dark:border-zinc-800">
          <div>
            <p className="text-xs text-zinc-500">Battery</p>
            <p className="text-lg font-medium">
              {stat.battery ? `${stat.battery.pct}%` : "—"}
              {stat.battery?.charging && (
                <span className="ml-1 text-xs text-emerald-600 dark:text-emerald-400">charging</span>
              )}
            </p>
          </div>
          <div>
            <p className="text-xs text-zinc-500">Device clock</p>
            <p className="text-lg font-medium">
              {stat.time > 0 ? new Date(stat.time * 1000).toLocaleTimeString() : "unset"}
            </p>
            <p className="text-xs text-zinc-500">
              {stat.time > 0 ? formatDrift(stat.time - statAt) : "no time since boot"}
            </p>
          </div>
          <div>
            <p className="text-xs text-zinc-500">Reviews today</p>
            <p className="text-lg font-medium">{stat.reviews_today}</p>
          </div>
          <div>
            <p className="text-xs text-zinc-500">Firmware</p>
            <p className="truncate text-lg font-medium">{stat.fw ?? "—"}</p>
          </div>
        </section>
      )}

      <section className="mt-8">
        <h2 className="text-sm font-semibold">Decks</h2>
        {!rows && (
          <p className="mt-2 text-sm text-zinc-500">
            Connect to list what is installed on the device.
          </p>
        )}
        {rows && rows.length === 0 && (
          <p className="mt-2 text-sm text-zinc-500">
            No decks on the device and none stored locally.
          </p>
        )}
        {rows && rows.length > 0 && (
          <table className="mt-2 w-full border-collapse text-sm">
            <thead>
              <tr className="border-b border-zinc-200 text-left text-xs text-zinc-500 dark:border-zinc-800">
                <th className="py-1 pr-2">Deck</th>
                <th className="py-1 pr-2">Cards</th>
                <th className="py-1 pr-2">Size</th>
                <th className="py-1 pr-2">Queue</th>
                <th className="py-1 pr-2">Status</th>
                <th className="py-1" />
              </tr>
            </thead>
            <tbody>
              {rows.map((row) => (
                <tr key={row.slug} className="border-b border-zinc-100 align-top dark:border-zinc-900">
                  <td className="py-2 pr-2">
                    <Link href={`/decks/${row.slug}`} className="font-medium hover:underline">
                      {row.name}
                    </Link>
                    <p className="text-xs text-zinc-500">
                      {row.slug}
                      {row.lang && ` · ${row.lang}`}
                    </p>
                    {row.device?.error && (
                      <p className="text-xs text-red-600">unreadable: {row.device.error}</p>
                    )}
                    {row.localError && (
                      <p className="text-xs text-amber-600">local: {row.localError}</p>
                    )}
                  </td>
                  <td className="py-2 pr-2">{row.device ? row.device.cards : "—"}</td>
                  <td className="py-2 pr-2">{row.device ? bytes(row.device.size) : "—"}</td>
                  <td className="py-2 pr-2 text-xs">
                    {row.counts
                      ? `${row.counts.due} due · ${row.counts.learning} learning · ${row.counts.fresh} new`
                      : "—"}
                  </td>
                  <td className={`py-2 pr-2 text-xs ${STATUS_CLASS[row.status]}`}>{row.status}</td>
                  <td className="py-2">
                    <div className="flex gap-2">
                      {row.local && (
                        <button
                          onClick={() => handlePushDeck(row.slug)}
                          disabled={!client || busy}
                          className="rounded border border-zinc-300 px-2 py-1 text-xs disabled:opacity-40 dark:border-zinc-700"
                        >
                          Push
                        </button>
                      )}
                      {row.device && (
                        <button
                          onClick={() => handleDeleteDeck(row.device!.path)}
                          disabled={!client || busy}
                          className="rounded border border-red-300 px-2 py-1 text-xs text-red-600 disabled:opacity-40 dark:border-red-900"
                        >
                          Delete
                        </button>
                      )}
                    </div>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </section>

      <div className="mt-8 h-72 overflow-y-auto rounded-lg border border-zinc-200 bg-white p-3 font-mono text-xs dark:border-zinc-800 dark:bg-zinc-900">
        {log.length === 0 && <p className="text-zinc-400">No activity yet.</p>}
        {log.map((entry, i) => (
          <p
            key={i}
            className={
              entry.kind === "error"
                ? "text-red-600"
                : entry.kind === "progress"
                  ? "text-zinc-400"
                  : "text-zinc-700 dark:text-zinc-300"
            }
          >
            {entry.text}
          </p>
        ))}
      </div>
    </div>
  );
}
