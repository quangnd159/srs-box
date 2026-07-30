import { describe, expect, test } from "bun:test";
import { crc32 } from "../lib/compiler/crc32";
import { LineReader } from "../lib/serial/lineReader";
import { DeviceClient, DeviceError } from "../lib/serial/protocol";
import type { SerialTransport } from "../lib/serial/transport";
import { makeDuplexPair } from "./mockDuplex";

async function writeLine(t: SerialTransport, s: string): Promise<void> {
  await t.write(new TextEncoder().encode(s + "\n"));
}

function crcHex(data: Uint8Array): string {
  return crc32(data).toString(16).padStart(8, "0");
}

describe("DeviceClient over a mock duplex stream", () => {
  test("@ping resolves on @ok", async () => {
    const { host, device, close } = makeDuplexPair();
    const client = new DeviceClient(host);
    const deviceReader = new LineReader(device);

    const devicePromise = (async () => {
      const line = await deviceReader.readLine();
      expect(line).toBe("@ping");
      await writeLine(device, "@ok pong");
    })();

    await client.ping();
    await devicePromise;
    close();
  });

  test("ignores interleaved ESP_LOG lines while awaiting a reply", async () => {
    const { host, device, close } = makeDuplexPair();
    const client = new DeviceClient(host);
    const deviceReader = new LineReader(device);

    const devicePromise = (async () => {
      await deviceReader.readLine(); // "@ping"
      await writeLine(device, "I (1234) wifi: some unrelated log line");
      await writeLine(device, "I (1235) app_main: another log line");
      await writeLine(device, "@ok pong");
    })();

    await client.ping(); // must not throw or hang on the log lines
    await devicePromise;
    close();
  });

  test("@fput handshake: header, ack, byte stream, final ack", async () => {
    const { host, device, close } = makeDuplexPair();
    const client = new DeviceClient(host);
    const deviceReader = new LineReader(device);

    const payload = new TextEncoder().encode("hello deck bytes".repeat(50));

    const devicePromise = (async () => {
      const header = await deviceReader.readLine();
      const [cmd, path, nbytesStr, crcHexGiven] = header.split(" ");
      expect(cmd).toBe("@fput");
      expect(path).toBe("decks/hsk1.srs");
      const nbytes = Number(nbytesStr);
      expect(nbytes).toBe(payload.length);

      await writeLine(device, "@ok send");

      const received = await deviceReader.readExact(nbytes);
      expect(crcHex(received)).toBe(crcHexGiven);
      expect(Buffer.from(received).equals(Buffer.from(payload))).toBe(true);

      await writeLine(device, `@ok fput ${path} ${nbytes}`);
    })();

    const progress: Array<[number, number]> = [];
    await client.fput("decks/hsk1.srs", payload, (sent, total) => progress.push([sent, total]));
    await devicePromise;

    expect(progress.length).toBeGreaterThan(0);
    expect(progress.at(-1)).toEqual([payload.length, payload.length]);
    close();
  });

  test("@fput surfaces a device-reported error", async () => {
    const { host, device, close } = makeDuplexPair();
    const client = new DeviceClient(host);
    const deviceReader = new LineReader(device);

    const devicePromise = (async () => {
      await deviceReader.readLine();
      await writeLine(device, "@err no space");
    })();

    await expect(client.fput("decks/hsk1.srs", new Uint8Array([1, 2, 3]))).rejects.toThrow(
      DeviceError,
    );
    await devicePromise;
    close();
  });

  test("@fget counts raw bytes exactly, independent of embedded newline-like bytes", async () => {
    const { host, device, close } = makeDuplexPair();
    const client = new DeviceClient(host);
    const deviceReader = new LineReader(device);

    // Deliberately include 0x0a bytes inside the payload to prove the
    // reader counts exact bytes rather than treating the payload as lines.
    const payload = new Uint8Array([0x0a, 0x00, 0x0a, 0xff, 0x0a, 1, 2, 3, 0x0a]);

    const devicePromise = (async () => {
      const line = await deviceReader.readLine();
      expect(line).toBe("@fget revlog.bin");
      await writeLine(device, `@fget ${payload.length} ${crcHex(payload)}`);
      await device.write(payload);
    })();

    const received = await client.fget("revlog.bin");
    expect(Buffer.from(received).equals(Buffer.from(payload))).toBe(true);
    await devicePromise;
    close();
  });

  test("@fget rejects on a crc mismatch", async () => {
    const { host, device, close } = makeDuplexPair();
    const client = new DeviceClient(host);
    const deviceReader = new LineReader(device);

    const payload = new Uint8Array([1, 2, 3, 4]);

    const devicePromise = (async () => {
      await deviceReader.readLine();
      await writeLine(device, `@fget ${payload.length} deadbeef`);
      await device.write(payload);
    })();

    await expect(client.fget("revlog.bin")).rejects.toThrow(DeviceError);
    await devicePromise;
    close();
  });

  test("@fls parses entries and a truncation marker", async () => {
    const { host, device, close } = makeDuplexPair();
    const client = new DeviceClient(host);
    const deviceReader = new LineReader(device);

    const devicePromise = (async () => {
      await deviceReader.readLine();
      await writeLine(device, "@ok fls decks/hsk1.srs=45120 fonts/font_cjk_16.bin=12000 ...");
    })();

    const result = await client.fls();
    expect(result.truncated).toBe(true);
    expect(result.entries).toEqual([
      { path: "decks/hsk1.srs", size: 45120 },
      { path: "fonts/font_cjk_16.bin", size: 12000 },
    ]);
    await devicePromise;
    close();
  });

  test("@reboot resolves on @ok", async () => {
    const { host, device, close } = makeDuplexPair();
    const client = new DeviceClient(host);
    const deviceReader = new LineReader(device);

    const devicePromise = (async () => {
      await deviceReader.readLine();
      await writeLine(device, "@ok rebooting");
    })();

    await client.reboot();
    await devicePromise;
    close();
  });
});
