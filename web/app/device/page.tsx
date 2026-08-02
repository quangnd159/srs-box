"use client";

import Link from "next/link";
import { useEffect, useSyncExternalStore } from "react";
import { useHasMounted } from "@/lib/deck/useDeckStore";
import { connectionStore, type SyncStatus } from "@/lib/serial/connectionStore";

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

function bytes(n: number): string {
  return n < 1024 ? `${n} B` : `${(n / 1024).toFixed(1)} KB`;
}

/**
 * A view over lib/serial/connectionStore.ts, which owns the actual
 * connection so it survives navigating away from this page and back (see
 * that file's header for why: state used to live in useState here and died
 * on unmount, which is the bug this component used to have).
 */
export default function DevicePage() {
  const hasMounted = useHasMounted();
  const snapshot = useSyncExternalStore(
    connectionStore.subscribe,
    connectionStore.getSnapshot,
    connectionStore.getServerSnapshot,
  );

  // Re-attaches to a live connection with no picker when exactly one
  // previously-granted port is available; a no-op otherwise (see
  // ConnectionStore.tryAutoReconnect / WebSerialTransport.autoConnect).
  useEffect(() => {
    void connectionStore.tryAutoReconnect();
  }, []);

  const serialSupported =
    !hasMounted || typeof navigator === "undefined" ? true : "serial" in navigator;

  const { status, busy, log, stat, statAt, rows, pullSummary } = snapshot;
  const connected = status === "connected";
  const controlsDisabled = !connected || busy;

  function handleConnect() {
    void connectionStore.connect();
  }

  function handleDisconnect() {
    void connectionStore.disconnect();
  }

  function handleRefresh() {
    void connectionStore.refresh();
  }

  function handleSyncTime() {
    void connectionStore.syncTime();
  }

  function handlePushDeck(slug: string) {
    if (!confirm(`Push "${slug}" and rebuilt fonts, then reboot the device?`)) return;
    void connectionStore.pushDeck(slug);
  }

  function handlePushAll() {
    if (!confirm("Push every local deck and rebuilt fonts, then reboot the device?")) return;
    void connectionStore.pushAll();
  }

  function handleDeleteDeck(path: string) {
    if (!confirm(`Delete ${path} from the device? Its review history is kept in revlog.bin.`)) {
      return;
    }
    void connectionStore.deleteDeck(path);
  }

  function handlePullRevlog() {
    void connectionStore.pullRevlog();
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
        {!connected ? (
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
          disabled={controlsDisabled}
          className="rounded border border-zinc-300 px-4 py-1.5 text-sm disabled:opacity-40 dark:border-zinc-700"
        >
          Refresh
        </button>
        <button
          onClick={handleSyncTime}
          disabled={controlsDisabled}
          className="rounded border border-zinc-300 px-4 py-1.5 text-sm disabled:opacity-40 dark:border-zinc-700"
        >
          Sync clock
        </button>
        <button
          onClick={handlePushAll}
          disabled={controlsDisabled}
          className="rounded bg-zinc-900 px-4 py-1.5 text-sm font-medium text-white disabled:opacity-40 dark:bg-zinc-100 dark:text-zinc-900"
        >
          Push everything
        </button>
        <button
          onClick={handlePullRevlog}
          disabled={controlsDisabled}
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
                          disabled={controlsDisabled}
                          className="rounded border border-zinc-300 px-2 py-1 text-xs disabled:opacity-40 dark:border-zinc-700"
                        >
                          Push
                        </button>
                      )}
                      {row.device && (
                        <button
                          onClick={() => handleDeleteDeck(row.device!.path)}
                          disabled={controlsDisabled}
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
