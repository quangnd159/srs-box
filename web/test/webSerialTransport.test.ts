// Tests the WebSerial-facing bits of lib/serial/webSerialTransport.ts that
// don't require an actual browser: open() idempotency (never re-call
// port.open() on an already-open port, which the spec rejects with
// InvalidStateError) and autoConnect()'s "exactly one granted port, or
// don't guess" rule. Uses real ReadableStream/WritableStream (Bun has
// both) behind a fake SerialPortLike/SerialLike, and a temporary
// `navigator.serial`, standing in for the browser API per CLAUDE.md's
// "you cannot test WebSerial against real hardware" note.
import { afterEach, describe, expect, test } from "bun:test";
import {
  ESPRESSIF_USB_VENDOR_ID,
  WebSerialTransport,
  type SerialLike,
  type SerialPortLike,
} from "../lib/serial/webSerialTransport";

function fakePort(vendorId: number | undefined, alreadyOpen = false): SerialPortLike & { openCalls: number } {
  let readable: ReadableStream<Uint8Array> | null = alreadyOpen ? new ReadableStream() : null;
  let writable: WritableStream<Uint8Array> | null = alreadyOpen ? new WritableStream() : null;
  const listeners = new Map<string, Set<() => void>>();
  const port = {
    openCalls: 0,
    async open() {
      port.openCalls++;
      readable = new ReadableStream();
      writable = new WritableStream();
    },
    async close() {
      readable = null;
      writable = null;
    },
    get readable() {
      return readable;
    },
    get writable() {
      return writable;
    },
    getInfo: () => (vendorId === undefined ? {} : { usbVendorId: vendorId }),
    addEventListener: (type: string, cb: () => void) => {
      if (!listeners.has(type)) listeners.set(type, new Set());
      listeners.get(type)!.add(cb);
    },
    removeEventListener: (type: string, cb: () => void) => {
      listeners.get(type)?.delete(cb);
    },
    // test-only helper
    fireDisconnect: () => {
      for (const cb of listeners.get("disconnect") ?? []) cb();
    },
  };
  return port as unknown as SerialPortLike & { openCalls: number };
}

const originalNavigator = globalThis.navigator;

function stubSerial(serial: SerialLike | undefined): void {
  const value = { ...originalNavigator } as Navigator & { serial?: SerialLike };
  if (serial) value.serial = serial;
  else delete value.serial;
  Object.defineProperty(globalThis, "navigator", { value, configurable: true });
}

afterEach(() => {
  Object.defineProperty(globalThis, "navigator", { value: originalNavigator, configurable: true });
});

describe("WebSerialTransport.open", () => {
  test("opens a closed port", async () => {
    const port = fakePort(ESPRESSIF_USB_VENDOR_ID);
    await WebSerialTransport.open(port);
    expect(port.openCalls).toBe(1);
  });

  test("does not re-open an already-open port (idempotent reconnect)", async () => {
    const port = fakePort(ESPRESSIF_USB_VENDOR_ID, /* alreadyOpen */ true);
    await WebSerialTransport.open(port);
    expect(port.openCalls).toBe(0);
  });
});

describe("WebSerialTransport.autoConnect", () => {
  test("connects with no picker when exactly one granted port matches the vendor id", async () => {
    const match = fakePort(ESPRESSIF_USB_VENDOR_ID);
    stubSerial({
      requestPort: async () => {
        throw new Error("should not show a picker");
      },
      getPorts: async () => [match],
    });

    const transport = await WebSerialTransport.autoConnect();
    expect(transport).not.toBeNull();
    expect(match.openCalls).toBe(1);
  });

  test("returns null when no granted ports match", async () => {
    stubSerial({
      requestPort: async () => {
        throw new Error("unused");
      },
      getPorts: async () => [fakePort(0x1234)],
    });

    expect(await WebSerialTransport.autoConnect()).toBeNull();
  });

  test("returns null (does not guess) when several granted ports match", async () => {
    stubSerial({
      requestPort: async () => {
        throw new Error("unused");
      },
      getPorts: async () => [fakePort(ESPRESSIF_USB_VENDOR_ID), fakePort(ESPRESSIF_USB_VENDOR_ID)],
    });

    expect(await WebSerialTransport.autoConnect()).toBeNull();
  });

  test("returns null when navigator.serial is unavailable", async () => {
    stubSerial(undefined);
    expect(await WebSerialTransport.autoConnect()).toBeNull();
  });
});

describe("WebSerialTransport disconnect handling", () => {
  test("onDisconnect fires the callback and can be detached", async () => {
    const port = fakePort(ESPRESSIF_USB_VENDOR_ID) as SerialPortLike & {
      openCalls: number;
      fireDisconnect: () => void;
    };
    const transport = await WebSerialTransport.open(port);

    let fired = 0;
    const detach = transport.onDisconnect(() => fired++);
    port.fireDisconnect();
    expect(fired).toBe(1);

    detach();
    port.fireDisconnect();
    expect(fired).toBe(1);
  });
});
