import { describe, expect, test } from "bun:test";
import { LineReader } from "../lib/serial/lineReader";
import { DeviceClient, DeviceError, parseDeviceStat } from "../lib/serial/protocol";
import type { SerialTransport } from "../lib/serial/transport";
import { makeDuplexPair } from "./mockDuplex";

async function writeLine(t: SerialTransport, s: string): Promise<void> {
  await t.write(new TextEncoder().encode(s + "\n"));
}

// Shape as main.cpp's on_stat_query() emits it.
const FULL_STAT =
  '{"fw":"1.4.2","battery":{"pct":73,"charging":true},"time":1785000000,"reviews_today":42,' +
  '"decks":[{"slug":"hsk1","name":"HSK 1","lang":"zh","cards":500,"learning":3,"due":18,"fresh":40},' +
  '{"slug":"fr-a1","name":"French A1","lang":"fr","cards":120,"learning":0,"due":2,"fresh":7}]}';

describe("@stat", () => {
  test("parses a full reply over the mock duplex stream", async () => {
    const { host, device, close } = makeDuplexPair();
    const client = new DeviceClient(host);
    const deviceReader = new LineReader(device);

    const devicePromise = (async () => {
      expect(await deviceReader.readLine()).toBe("@stat");
      await writeLine(device, `@ok stat ${FULL_STAT}`);
    })();

    const stat = await client.stat();
    expect(stat.fw).toBe("1.4.2");
    expect(stat.battery).toEqual({ pct: 73, charging: true });
    expect(stat.time).toBe(1785000000);
    expect(stat.reviews_today).toBe(42);
    expect(stat.decks).toHaveLength(2);
    expect(stat.decks[0]).toEqual({
      slug: "hsk1",
      name: "HSK 1",
      lang: "zh",
      cards: 500,
      learning: 3,
      due: 18,
      fresh: 40,
    });
    await devicePromise;
    close();
  });

  test("skips ESP_LOG lines before the reply, like every other command", async () => {
    const { host, device, close } = makeDuplexPair();
    const client = new DeviceClient(host);
    const deviceReader = new LineReader(device);

    const devicePromise = (async () => {
      await deviceReader.readLine();
      await writeLine(device, "I (900) main: refreshing counts");
      await writeLine(device, `@ok stat ${FULL_STAT}`);
    })();

    expect((await client.stat()).reviews_today).toBe(42);
    await devicePromise;
    close();
  });

  test("throws DeviceError on @err, so the page can hide the summary", async () => {
    const { host, device, close } = makeDuplexPair();
    const client = new DeviceClient(host);
    const deviceReader = new LineReader(device);

    const devicePromise = (async () => {
      await deviceReader.readLine();
      await writeLine(device, "@err unknown command");
    })();

    await expect(client.stat()).rejects.toThrow(DeviceError);
    await devicePromise;
    close();
  });

  test("throws DeviceError on malformed JSON", async () => {
    const { host, device, close } = makeDuplexPair();
    const client = new DeviceClient(host);
    const deviceReader = new LineReader(device);

    const devicePromise = (async () => {
      await deviceReader.readLine();
      // What a truncated 2048-byte @stat buffer looks like from the host side.
      await writeLine(device, '@ok stat {"time":123,"decks":[{"slug":"hsk');
    })();

    await expect(client.stat()).rejects.toThrow(DeviceError);
    await devicePromise;
    close();
  });

  test("throws DeviceError on an @ok reply that isn't a stat", async () => {
    const { host, device, close } = makeDuplexPair();
    const client = new DeviceClient(host);
    const deviceReader = new LineReader(device);

    const devicePromise = (async () => {
      await deviceReader.readLine();
      await writeLine(device, "@ok pong");
    })();

    await expect(client.stat()).rejects.toThrow(DeviceError);
    await devicePromise;
    close();
  });
});

describe("parseDeviceStat tolerance", () => {
  test("missing fw and battery degrade to undefined, not a throw", () => {
    const stat = parseDeviceStat('{"time":0,"reviews_today":0,"decks":[]}');
    expect(stat.fw).toBeUndefined();
    expect(stat.battery).toBeUndefined();
    expect(stat.time).toBe(0);
    expect(stat.decks).toEqual([]);
  });

  test("unknown fields are ignored and absent numbers default to 0", () => {
    const stat = parseDeviceStat(
      '{"time":5,"future_field":{"x":1},"decks":[{"slug":"a","name":"A"}]}',
    );
    expect(stat.reviews_today).toBe(0);
    expect(stat.decks[0]).toEqual({
      slug: "a",
      name: "A",
      lang: "",
      cards: 0,
      learning: 0,
      due: 0,
      fresh: 0,
    });
  });

  test("a non-array decks field yields an empty list rather than throwing", () => {
    expect(parseDeviceStat('{"time":1,"decks":"nope"}').decks).toEqual([]);
  });

  test("non-object deck entries are skipped", () => {
    expect(parseDeviceStat('{"time":1,"decks":[null,7,{"slug":"a"}]}').decks).toHaveLength(1);
  });

  test("charging is strictly boolean true, never a truthy string", () => {
    expect(parseDeviceStat('{"battery":{"pct":10,"charging":"yes"},"time":1}').battery).toEqual({
      pct: 10,
      charging: false,
    });
  });

  test("a non-object root throws", () => {
    expect(() => parseDeviceStat("[1,2,3]")).toThrow(DeviceError);
    expect(() => parseDeviceStat("42")).toThrow(DeviceError);
  });
});
