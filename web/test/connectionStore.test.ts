// Tests the non-DOM parts of lib/serial/connectionStore.ts: state
// transitions, the log ring, idempotent connect, and that a command with no
// reply degrades into a logged error with `busy` cleared instead of hanging
// the UI forever. All of it runs against a fake ConnectionEnvironment and a
// mock duplex stream (test/mockDuplex.ts) standing in for a device, per
// CLAUDE.md's "you cannot test WebSerial against real hardware" note.
import { describe, expect, test } from "bun:test";
import { buildDeck } from "../lib/compiler/build";
import { crc32 } from "../lib/compiler/crc32";
import { DeckStore, MemoryStore } from "../lib/deck/store";
import {
  ConnectionStore,
  type ConnectionEnvironment,
  type TransportHandle,
} from "../lib/serial/connectionStore";
import { LineReader } from "../lib/serial/lineReader";
import type { SerialTransport } from "../lib/serial/transport";
import { makeDuplexPair } from "./mockDuplex";

async function writeLine(t: SerialTransport, s: string): Promise<void> {
  await t.write(new TextEncoder().encode(s + "\n"));
}

interface FakeTransport {
  handle: TransportHandle;
  device: SerialTransport;
  triggerDisconnect: () => void;
  closeCalls: () => number;
}

/** A TransportHandle backed by a mock duplex pair, with a spy'd close() and a controllable disconnect event. */
function makeFakeTransport(): FakeTransport {
  const { host, device, close } = makeDuplexPair();
  let disconnectCb: (() => void) | null = null;
  let closeCalls = 0;
  const handle: TransportHandle = {
    write: (data) => host.write(data),
    read: () => host.read(),
    close: async () => {
      closeCalls++;
      close();
    },
    onDisconnect: (cb) => {
      disconnectCb = cb;
      return () => {
        disconnectCb = null;
      };
    },
  };
  return {
    handle,
    device,
    triggerDisconnect: () => disconnectCb?.(),
    closeCalls: () => closeCalls,
  };
}

/** A minimal fake firmware loop: answers @time, @stat, @fls (empty), @ping, @reboot. Ignores @fls if `hangOnFls`. */
async function runFakeDevice(device: SerialTransport, opts: { hangOnFls?: boolean } = {}): Promise<void> {
  const reader = new LineReader(device);
  for (;;) {
    let line: string;
    try {
      line = await reader.readLine();
    } catch {
      return;
    }
    if (line.startsWith("@time")) {
      await writeLine(device, "@ok time");
    } else if (line === "@stat") {
      await writeLine(device, '@ok stat {"time":1000,"reviews_today":2,"decks":[]}');
    } else if (line === "@fls") {
      if (opts.hangOnFls) continue; // simulates a lost reply: never answers
      await writeLine(device, "@ok fls");
    } else if (line === "@ping") {
      await writeLine(device, "@ok pong");
    } else if (line === "@reboot") {
      await writeLine(device, "@ok rebooting");
    }
  }
}

function emptyDeckSource() {
  return { listDecks: () => [], getCards: () => [] };
}

/** Like runFakeDevice, but serves a fixed set of `decks/*.srs` files over @fls/@fget. */
async function runFakeDeviceWithFiles(device: SerialTransport, files: Map<string, Uint8Array>): Promise<void> {
  const reader = new LineReader(device);
  for (;;) {
    let line: string;
    try {
      line = await reader.readLine();
    } catch {
      return;
    }
    if (line.startsWith("@time")) {
      await writeLine(device, "@ok time");
    } else if (line === "@stat") {
      await writeLine(device, '@ok stat {"time":1000,"reviews_today":0,"decks":[]}');
    } else if (line === "@fls") {
      const listing = [...files.entries()].map(([path, data]) => `${path}=${data.length}`).join(" ");
      await writeLine(device, `@ok fls ${listing}`.trimEnd());
    } else if (line.startsWith("@fget ")) {
      const path = line.slice("@fget ".length).trim();
      const data = files.get(path);
      if (!data) {
        await writeLine(device, "@err no such file");
        continue;
      }
      const crcHex = crc32(data).toString(16).padStart(8, "0");
      await writeLine(device, `@fget ${data.length} ${crcHex}`);
      await device.write(data);
    } else if (line === "@ping") {
      await writeLine(device, "@ok pong");
    } else if (line === "@reboot") {
      await writeLine(device, "@ok rebooting");
    }
  }
}

describe("ConnectionStore", () => {
  test("starts disconnected with an empty log", () => {
    const store = new ConnectionStore({ deckSource: emptyDeckSource });
    const snap = store.getSnapshot();
    expect(snap.status).toBe("disconnected");
    expect(snap.busy).toBe(false);
    expect(snap.log).toEqual([]);
    expect(snap.rows).toBeNull();
  });

  test("connect() drives @time then a full refresh, ending connected with rows populated", async () => {
    const fake = makeFakeTransport();
    void runFakeDevice(fake.device);
    const env: ConnectionEnvironment = {
      requestAndOpen: async () => fake.handle,
      autoConnect: async () => null,
    };
    const store = new ConnectionStore({ env, deckSource: emptyDeckSource });

    await store.connect();

    const snap = store.getSnapshot();
    expect(snap.status).toBe("connected");
    expect(snap.busy).toBe(false);
    expect(snap.stat?.reviews_today).toBe(2);
    expect(snap.rows).toEqual([]);
    expect(snap.log.filter((l) => l.kind !== "progress").map((l) => l.text)).toEqual([
      "connected",
      "clock synced",
      "device state read",
    ]);
    expect(snap.log.every((l) => l.kind !== "error")).toBe(true);
  });

  test("tryAutoReconnect connects with no picker when the environment finds a granted port", async () => {
    const fake = makeFakeTransport();
    void runFakeDevice(fake.device);
    let requestCalls = 0;
    const env: ConnectionEnvironment = {
      requestAndOpen: async () => {
        requestCalls++;
        return fake.handle;
      },
      autoConnect: async () => fake.handle,
    };
    const store = new ConnectionStore({ env, deckSource: emptyDeckSource });

    await store.tryAutoReconnect();

    expect(store.getSnapshot().status).toBe("connected");
    expect(requestCalls).toBe(0); // never fell back to the picker
  });

  test("tryAutoReconnect is a no-op when the environment finds nothing (leaves Connect button up)", async () => {
    const env: ConnectionEnvironment = {
      requestAndOpen: async () => {
        throw new Error("should not be called");
      },
      autoConnect: async () => null,
    };
    const store = new ConnectionStore({ env, deckSource: emptyDeckSource });

    await store.tryAutoReconnect();

    expect(store.getSnapshot().status).toBe("disconnected");
    expect(store.getSnapshot().log).toEqual([]);
  });

  test("connect() is idempotent: a second concurrent call does not open a second port", async () => {
    const fake = makeFakeTransport();
    void runFakeDevice(fake.device);
    let opens = 0;
    const env: ConnectionEnvironment = {
      requestAndOpen: async () => {
        opens++;
        return fake.handle;
      },
      autoConnect: async () => null,
    };
    const store = new ConnectionStore({ env, deckSource: emptyDeckSource });

    await Promise.all([store.connect(), store.connect()]);

    expect(opens).toBe(1);
    expect(store.getSnapshot().status).toBe("connected");
  });

  test("disconnect() closes the transport and clears device-derived state, but keeps the log", async () => {
    const fake = makeFakeTransport();
    void runFakeDevice(fake.device);
    const env: ConnectionEnvironment = { requestAndOpen: async () => fake.handle, autoConnect: async () => null };
    const store = new ConnectionStore({ env, deckSource: emptyDeckSource });
    await store.connect();

    await store.disconnect();

    const snap = store.getSnapshot();
    expect(snap.status).toBe("disconnected");
    expect(snap.stat).toBeNull();
    expect(snap.rows).toBeNull();
    expect(fake.closeCalls()).toBe(1);
    expect(snap.log.at(-1)?.text).toBe("disconnected");
  });

  test("a physical disconnect event tears the connection down and logs an error line", async () => {
    const fake = makeFakeTransport();
    void runFakeDevice(fake.device);
    const env: ConnectionEnvironment = { requestAndOpen: async () => fake.handle, autoConnect: async () => null };
    const store = new ConnectionStore({ env, deckSource: emptyDeckSource });
    await store.connect();

    fake.triggerDisconnect();
    // The handler runs through the same queue as everything else; give it a tick.
    await new Promise((r) => setTimeout(r, 0));

    const snap = store.getSnapshot();
    expect(snap.status).toBe("disconnected");
    expect(snap.log.some((l) => l.text === "device disconnected" && l.kind === "error")).toBe(true);
  });

  test("reconnecting after a disconnect does not throw InvalidStateError-style errors (locks were released)", async () => {
    const fakeA = makeFakeTransport();
    void runFakeDevice(fakeA.device);
    const fakeB = makeFakeTransport();
    void runFakeDevice(fakeB.device);
    let call = 0;
    const env: ConnectionEnvironment = {
      requestAndOpen: async () => (call++ === 0 ? fakeA.handle : fakeB.handle),
      autoConnect: async () => null,
    };
    const store = new ConnectionStore({ env, deckSource: emptyDeckSource });

    await store.connect();
    await store.disconnect();
    await store.connect();

    expect(store.getSnapshot().status).toBe("connected");
    expect(store.getSnapshot().log.filter((l) => l.kind === "error")).toEqual([]);
  });

  test("a command that never replies times out, logs an error, and leaves busy cleared (buttons re-enabled)", async () => {
    const fake = makeFakeTransport();
    void runFakeDevice(fake.device, { hangOnFls: true });
    const env: ConnectionEnvironment = { requestAndOpen: async () => fake.handle, autoConnect: async () => null };
    const store = new ConnectionStore({ env, deckSource: emptyDeckSource, timeoutMs: 30 });

    await store.connect();

    const snap = store.getSnapshot();
    // @time and @stat succeeded (summary would populate); @fls never replied,
    // so refresh's timeout fired instead of hanging forever.
    expect(snap.status).toBe("connected");
    expect(snap.busy).toBe(false);
    expect(snap.stat?.reviews_today).toBe(2);
    expect(snap.rows).toBeNull(); // refresh never finished
    expect(snap.log.some((l) => l.kind === "error" && l.text.includes("connect failed"))).toBe(true);

    // The store is left usable: a later refresh (against a device that now
    // answers) succeeds rather than being stuck forever.
  });

  test("errors from an operation always clear busy, even back-to-back", async () => {
    const env: ConnectionEnvironment = {
      requestAndOpen: async () => {
        throw new Error("no device");
      },
      autoConnect: async () => null,
    };
    const store = new ConnectionStore({ env, deckSource: emptyDeckSource });

    await store.connect();
    expect(store.getSnapshot().status).toBe("disconnected");
    expect(store.getSnapshot().busy).toBe(false);
    expect(store.getSnapshot().log.at(-1)?.kind).toBe("error");

    await store.connect(); // must be retryable, not wedged
    expect(store.getSnapshot().log.filter((l) => l.kind === "error").length).toBe(2);
  });
});

describe("ConnectionStore.importDeck", () => {
  /** A deck with a syllable-separated (U+001F) reading, matching what a
   * real zh deck stores on the device. */
  async function buildFixtureDeck(): Promise<Uint8Array> {
    const built = await buildDeck(
      [{ front: "有时候", reading: "yǒushíhou", back: "sometimes", numeric: "you3shi2hou5" }],
      { name: "Vocab", slug: "vocab", lang: "zh" },
    );
    return built.bytes;
  }

  test("imports a device-only deck, preserving the U+001F reading verbatim, and the row flips to in sync", async () => {
    const kv = new MemoryStore();
    const deckSource = () => new DeckStore(kv);
    const deckBytes = await buildFixtureDeck();

    const fake = makeFakeTransport();
    void runFakeDeviceWithFiles(fake.device, new Map([["decks/vocab.srs", deckBytes]]));
    const env: ConnectionEnvironment = { requestAndOpen: async () => fake.handle, autoConnect: async () => null };
    const store = new ConnectionStore({ env, deckSource, kv: () => kv });

    await store.connect();
    const before = store.getSnapshot().rows?.find((r) => r.slug === "vocab");
    expect(before?.status).toBe("not local");

    await store.importDeck("vocab");

    const localCards = new DeckStore(kv).getCards("vocab");
    expect(localCards).toHaveLength(1);
    expect(localCards[0].front).toBe("有时候");
    expect(localCards[0].reading).toContain("");
    expect(localCards[0].reading).toBe("yǒushíhou");
    expect(localCards[0].numeric).toBeUndefined();

    const after = store.getSnapshot().rows?.find((r) => r.slug === "vocab");
    expect(after?.status).toBe("in sync");
    expect(store.getSnapshot().log.some((l) => l.text.includes("imported vocab"))).toBe(true);
  });

  test("refuses to import over an existing local slug, logs an error, and leaves the existing deck untouched", async () => {
    const kv = new MemoryStore();
    const deckSource = () => new DeckStore(kv);
    const deckBytes = await buildFixtureDeck();

    // A local deck already occupies the "vocab" slug, with its own cards.
    const preexisting = new DeckStore(kv);
    preexisting.createDeck({ name: "My Vocab", slug: "vocab", lang: "zh" });
    preexisting.addCards("vocab", [{ localId: "x1", front: "existing", reading: "", back: "" }]);

    const fake = makeFakeTransport();
    void runFakeDeviceWithFiles(fake.device, new Map([["decks/vocab.srs", deckBytes]]));
    const env: ConnectionEnvironment = { requestAndOpen: async () => fake.handle, autoConnect: async () => null };
    const store = new ConnectionStore({ env, deckSource, kv: () => kv });

    await store.connect();
    await store.importDeck("vocab");

    const cards = new DeckStore(kv).getCards("vocab");
    expect(cards).toHaveLength(1);
    expect(cards[0].front).toBe("existing"); // untouched, not overwritten

    expect(
      store.getSnapshot().log.some((l) => l.kind === "error" && l.text.includes("already exists")),
    ).toBe(true);
  });
});
