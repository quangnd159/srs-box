"use client";

import { useState } from "react";
import { buildDeck } from "@/lib/compiler/build";
import { useDeckStore } from "@/lib/deck/useDeckStore";
import { DeviceClient } from "@/lib/serial/protocol";
import { WebSerialTransport } from "@/lib/serial/webSerialTransport";

interface LogEntry {
  text: string;
  kind: "info" | "error" | "progress";
}

export default function SyncPage() {
  const store = useDeckStore();
  const [client, setClient] = useState<DeviceClient | null>(null);
  const [transport, setTransport] = useState<WebSerialTransport | null>(null);
  const [log, setLog] = useState<LogEntry[]>([]);
  const [busy, setBusy] = useState(false);

  function push(text: string, kind: LogEntry["kind"] = "info") {
    setLog((l) => [...l, { text, kind }]);
  }

  async function handleConnect() {
    try {
      const t = await WebSerialTransport.requestAndOpen();
      const c = new DeviceClient(t);
      setTransport(t);
      setClient(c);
      push("connected");
    } catch (err) {
      push(`connect failed: ${(err as Error).message}`, "error");
    }
  }

  async function handleDisconnect() {
    await transport?.close();
    setTransport(null);
    setClient(null);
    push("disconnected");
  }

  async function handlePing() {
    if (!client) return;
    try {
      await client.ping();
      push("@ping ok");
    } catch (err) {
      push(`@ping failed: ${(err as Error).message}`, "error");
    }
  }

  async function handleSyncTime() {
    if (!client) return;
    try {
      await client.syncTime(Math.floor(Date.now() / 1000));
      push("clock synced");
    } catch (err) {
      push(`@time failed: ${(err as Error).message}`, "error");
    }
  }

  async function handlePushAll() {
    if (!client) return;
    setBusy(true);
    try {
      const decks = store.listDecks();
      if (decks.length === 0) {
        push("no decks to push", "error");
        return;
      }

      const compiled = await Promise.all(
        decks.map(async (deck) => {
          const cards = store.getCards(deck.slug).filter((c) => c.front.trim());
          const result = await buildDeck(cards, {
            name: deck.name,
            slug: deck.slug,
            lang: deck.lang,
          });
          return { deck, result };
        }),
      );

      for (const { deck, result } of compiled) {
        push(`pushing decks/${deck.slug}.srs (${result.bytes.length.toLocaleString()} bytes)`);
        await client.fput(`decks/${deck.slug}.srs`, result.bytes, (sent, total) => {
          push(`  ${sent}/${total} bytes`, "progress");
        });
        push(`decks/${deck.slug}.srs ok`);
      }

      push("subsetting fonts over the union of all decks' glyphs...");
      const res = await fetch("/api/font", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ deckGlyphSets: compiled.map((c) => c.result.glyphs.join("")) }),
      });
      if (!res.ok) {
        const body = await res.json().catch(() => ({ error: res.statusText }));
        throw new Error(body.error ?? res.statusText);
      }
      const { fonts } = (await res.json()) as { fonts: Record<string, string> };

      for (const [size, base64] of Object.entries(fonts)) {
        const bytes = Uint8Array.from(atob(base64), (c) => c.charCodeAt(0));
        push(`pushing fonts/font_cjk_${size}.bin (${bytes.length.toLocaleString()} bytes)`);
        await client.fput(`fonts/font_cjk_${size}.bin`, bytes, (sent, total) => {
          push(`  ${sent}/${total} bytes`, "progress");
        });
        push(`fonts/font_cjk_${size}.bin ok`);
      }

      await client.reboot();
      push("rebooting device to apply pushed content");
    } catch (err) {
      push(`push failed: ${(err as Error).message}`, "error");
    } finally {
      setBusy(false);
    }
  }

  async function handlePullRevlog() {
    if (!client) return;
    setBusy(true);
    try {
      push("pulling revlog.bin");
      const data = await client.fget("revlog.bin", (received, total) => {
        push(`  ${received}/${total} bytes`, "progress");
      });
      const blob = new Blob([data.buffer as ArrayBuffer], {
        type: "application/octet-stream",
      });
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = "revlog.bin";
      a.click();
      URL.revokeObjectURL(url);
      push(`revlog.bin ok, ${data.length.toLocaleString()} bytes, crc verified`);
    } catch (err) {
      push(`pull failed: ${(err as Error).message}`, "error");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className="mx-auto max-w-3xl px-6 py-10">
      <h1 className="text-2xl font-semibold">Sync</h1>
      <p className="mt-1 text-sm text-zinc-600 dark:text-zinc-400">
        Connects over WebSerial, matching docs/sync-protocol.md. Only Chromium-based
        browsers support Web Serial.
      </p>

      <div className="mt-6 flex flex-wrap gap-2">
        {!client ? (
          <button
            onClick={handleConnect}
            className="rounded bg-zinc-900 px-4 py-1.5 text-sm font-medium text-white dark:bg-zinc-100 dark:text-zinc-900"
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
          onClick={handlePing}
          disabled={!client || busy}
          className="rounded border border-zinc-300 px-4 py-1.5 text-sm disabled:opacity-40 dark:border-zinc-700"
        >
          @ping
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
          Push all decks + fonts, then reboot
        </button>
        <button
          onClick={handlePullRevlog}
          disabled={!client || busy}
          className="rounded border border-zinc-300 px-4 py-1.5 text-sm disabled:opacity-40 dark:border-zinc-700"
        >
          Pull review log
        </button>
      </div>

      <div className="mt-6 h-80 overflow-y-auto rounded-lg border border-zinc-200 bg-white p-3 font-mono text-xs dark:border-zinc-800 dark:bg-zinc-900">
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
