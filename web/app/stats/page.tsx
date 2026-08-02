"use client";

import Link from "next/link";
import { useMemo, useState } from "react";
import { browserStore } from "@/lib/deck/store";
import { useHasMounted } from "@/lib/deck/useDeckStore";
import { toAnkiCsv } from "@/lib/revlog/ankiExport";
import { parseRevlog } from "@/lib/revlog/parse";
import { loadRevlog, type RevlogSnapshot } from "../revlogStorage";
import {
  currentStreak,
  heatLevel,
  heatmapWeeks,
  lastDays,
  perDeckStats,
  ratingCounts,
  totals,
  withUnknownDecks,
} from "../statsView";

const HEATMAP_WEEKS = 26;
const RATING_LABELS = ["Again", "Hard", "Good", "Easy"];

// Neutral five-step scale, light and dark. Kept as literal class names so
// Tailwind's scanner sees them.
const HEAT_CLASS = [
  "bg-zinc-100 dark:bg-zinc-900",
  "bg-zinc-300 dark:bg-zinc-700",
  "bg-zinc-400 dark:bg-zinc-600",
  "bg-zinc-600 dark:bg-zinc-400",
  "bg-zinc-900 dark:bg-zinc-100",
];

// Sampled once when this module loads rather than during render, which must
// stay pure. Every "last N days" window on the page is anchored to it, so
// they all agree; a tab left open across the 4am rollover shows yesterday's
// window until it is reloaded, which is a fair trade for a stats page.
const NOW_SECONDS = BigInt(Math.floor(Date.now() / 1000));

function download(name: string, text: string, type: string) {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  URL.revokeObjectURL(url);
}

export default function StatsPage() {
  const hasMounted = useHasMounted();
  const [uploaded, setUploaded] = useState<{ snapshot: RevlogSnapshot; name: string } | null>(null);
  const [error, setError] = useState<string | null>(null);

  // Derived, not copied into state: a synchronous localStorage read that
  // recomputes for free when the mount flag flips, which is the earliest
  // point localStorage may be touched at all (see useDeckStore).
  const stored = useMemo(() => (hasMounted ? loadRevlog(browserStore()) : null), [hasMounted]);

  const snapshot = uploaded?.snapshot ?? stored;
  const source = uploaded
    ? `${uploaded.name}, read locally`
    : stored
      ? `pulled ${new Date(stored.pulledAt).toLocaleString()}`
      : null;

  async function handleUpload(file: File) {
    setError(null);
    try {
      const parsed = parseRevlog(new Uint8Array(await file.arrayBuffer()));
      setUploaded({
        name: file.name,
        snapshot: {
          pulledAt: new Date().toISOString(),
          version: parsed.version,
          entries: parsed.entries,
          // An uploaded revlog.bin carries timestamps and ratings but no deck
          // names, so keep whatever card map the last device pull left behind.
          cards: stored?.cards ?? new Map(),
        },
      });
    } catch (err) {
      setError((err as Error).message);
    }
  }

  const entries = useMemo(() => snapshot?.entries ?? [], [snapshot]);
  const now = NOW_SECONDS;

  const summary = useMemo(() => totals(entries), [entries]);
  const streak = useMemo(() => currentStreak(entries, now), [entries, now]);
  const weeks = useMemo(() => heatmapWeeks(entries, now, HEATMAP_WEEKS), [entries, now]);
  const recent = useMemo(() => lastDays(entries, now, 14), [entries, now]);
  const decks = useMemo(
    () => perDeckStats(entries, snapshot?.cards ?? new Map()),
    [entries, snapshot],
  );
  const ratings = useMemo(() => ratingCounts(entries), [entries]);

  const heatMax = useMemo(
    () => weeks.flat().reduce((m, cell) => Math.max(m, cell?.count ?? 0), 0),
    [weeks],
  );
  const recentMax = useMemo(() => recent.reduce((m, d) => Math.max(m, d.count), 1), [recent]);

  function handleExportCsv() {
    if (!snapshot) return;
    const cards = withUnknownDecks(entries, snapshot.cards);
    download("srsbox-revlog.csv", toAnkiCsv(entries, cards), "text/csv");
  }

  const uploadControl = (
    <label className="inline-flex cursor-pointer items-center rounded border border-zinc-300 px-3 py-1.5 text-sm dark:border-zinc-700">
      Load a revlog.bin
      <input
        type="file"
        accept=".bin"
        className="hidden"
        onChange={(e) => {
          const file = e.target.files?.[0];
          if (file) void handleUpload(file);
          e.target.value = "";
        }}
      />
    </label>
  );

  return (
    <div className="mx-auto max-w-4xl px-6 py-10">
      <h1 className="text-2xl font-semibold">Stats</h1>
      <p className="mt-1 text-sm text-zinc-600 dark:text-zinc-400">
        Everything here comes from the review log, the device&apos;s source of truth.
        {source && <span className="text-zinc-500"> Source: {source}.</span>}
      </p>

      {error && <p className="mt-3 text-sm text-red-600">{error}</p>}

      {!hasMounted ? (
        <p className="mt-8 text-sm text-zinc-500">Loading…</p>
      ) : entries.length === 0 ? (
        <div className="mt-8 rounded-lg border border-zinc-200 p-6 dark:border-zinc-800">
          <p className="text-sm text-zinc-600 dark:text-zinc-400">
            No review log yet. Connect the device on{" "}
            <Link href="/device" className="underline">
              /device
            </Link>{" "}
            and press &ldquo;Pull review log&rdquo;, or load a{" "}
            <span className="font-mono text-xs">revlog.bin</span> you already have.
          </p>
          <div className="mt-4">{uploadControl}</div>
        </div>
      ) : (
        <>
          <section className="mt-6 grid grid-cols-2 gap-4 rounded-lg border border-zinc-200 p-4 sm:grid-cols-4 dark:border-zinc-800">
            <div>
              <p className="text-xs text-zinc-500">Reviews</p>
              <p className="text-lg font-medium">{summary.reviews.toLocaleString()}</p>
            </div>
            <div>
              <p className="text-xs text-zinc-500">Distinct cards</p>
              <p className="text-lg font-medium">{summary.distinctCards.toLocaleString()}</p>
            </div>
            <div>
              <p className="text-xs text-zinc-500">First review</p>
              <p className="text-lg font-medium">{summary.firstDay}</p>
            </div>
            <div>
              <p className="text-xs text-zinc-500">Current streak</p>
              <p className="text-lg font-medium">
                {streak} {streak === 1 ? "day" : "days"}
              </p>
            </div>
          </section>

          <section className="mt-8">
            <h2 className="text-sm font-semibold">Last {HEATMAP_WEEKS} weeks</h2>
            <div className="mt-2 flex gap-[3px] overflow-x-auto pb-1">
              {weeks.map((week, w) => (
                <div key={w} className="flex flex-col gap-[3px]">
                  {week.map((cell, d) =>
                    cell ? (
                      <div
                        key={d}
                        title={`${cell.label}: ${cell.count} ${cell.count === 1 ? "review" : "reviews"}`}
                        className={`h-3 w-3 rounded-sm ${HEAT_CLASS[heatLevel(cell.count, heatMax)]}`}
                      />
                    ) : (
                      <div key={d} className="h-3 w-3" />
                    ),
                  )}
                </div>
              ))}
            </div>
            <div className="mt-2 flex items-center gap-1 text-xs text-zinc-500">
              <span>Less</span>
              {HEAT_CLASS.map((cls, i) => (
                <span key={i} className={`h-3 w-3 rounded-sm ${cls}`} aria-hidden />
              ))}
              <span>More</span>
              <span className="ml-2">busiest day: {heatMax}</span>
            </div>
          </section>

          <section className="mt-8">
            <h2 className="text-sm font-semibold">Last 14 days</h2>
            <ul className="mt-2 flex flex-col gap-1">
              {recent.map((day) => (
                <li key={day.label} className="flex items-center gap-3 text-xs">
                  <span className="w-20 shrink-0 font-mono text-zinc-500">{day.label}</span>
                  <span
                    className="h-3 rounded-sm bg-zinc-400 dark:bg-zinc-600"
                    style={{ width: `${(day.count / recentMax) * 100}%` }}
                  />
                  <span className="text-zinc-500">{day.count || ""}</span>
                </li>
              ))}
            </ul>
          </section>

          <section className="mt-8">
            <h2 className="text-sm font-semibold">By deck</h2>
            <table className="mt-2 w-full border-collapse text-sm">
              <thead>
                <tr className="border-b border-zinc-200 text-left text-xs text-zinc-500 dark:border-zinc-800">
                  <th className="py-1 pr-2">Deck</th>
                  <th className="py-1 pr-2">Reviews</th>
                  <th className="py-1 pr-2">Cards</th>
                  <th className="py-1">Lapses</th>
                </tr>
              </thead>
              <tbody>
                {decks.map((row) => (
                  <tr key={row.deckSlug} className="border-b border-zinc-100 dark:border-zinc-900">
                    <td className="py-1 pr-2">{row.deckSlug}</td>
                    <td className="py-1 pr-2">{row.reviews.toLocaleString()}</td>
                    <td className="py-1 pr-2">{row.distinctCards.toLocaleString()}</td>
                    <td className="py-1">{row.lapses.toLocaleString()}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </section>

          <section className="mt-8">
            <h2 className="text-sm font-semibold">Ratings</h2>
            <ul className="mt-2 flex flex-col gap-1">
              {ratings.map((count, i) => (
                <li key={RATING_LABELS[i]} className="flex items-center gap-3 text-xs">
                  <span className="w-20 shrink-0 text-zinc-500">{RATING_LABELS[i]}</span>
                  <span
                    className="h-3 rounded-sm bg-zinc-400 dark:bg-zinc-600"
                    style={{ width: `${(count / Math.max(...ratings, 1)) * 100}%` }}
                  />
                  <span className="text-zinc-500">{count.toLocaleString()}</span>
                </li>
              ))}
            </ul>
          </section>

          <section className="mt-8 flex flex-wrap items-center gap-3">
            <button
              onClick={handleExportCsv}
              className="rounded bg-zinc-900 px-4 py-1.5 text-sm font-medium text-white dark:bg-zinc-100 dark:text-zinc-900"
            >
              Export Anki CSV
            </button>
            {uploadControl}
            <span className="text-xs text-zinc-500">
              Anki recomputes its own schedule from these rows; due dates are never synced.
            </span>
          </section>
        </>
      )}
    </div>
  );
}
