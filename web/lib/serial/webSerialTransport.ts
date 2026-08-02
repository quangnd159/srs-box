// Browser-only adapter: wraps the Web Serial API's SerialPort as the
// SerialTransport interface lib/serial/protocol.ts expects. Not unit
// tested here (WebSerial doesn't exist outside a browser); protocol.ts
// itself is tested against a mock in test/serialProtocol.test.ts, per
// CLAUDE.md's "you cannot test WebSerial against real hardware" note.
// connectionStore.ts is the one thing here that IS unit tested, against a
// fake SerialLike/SerialPortLike pair (see test/connectionStore.test.ts),
// which is why the browser-facing bits below are kept thin and behind these
// two interfaces rather than reaching for `navigator.serial` directly.
import type { SerialTransport } from "./transport";

// Espressif's USB vendor id (see CLAUDE.md and tools/devctl.py).
export const ESPRESSIF_USB_VENDOR_ID = 0x303a;

// Minimal Web Serial API surface this module needs. Not every browser's
// lib.dom.d.ts ships these types yet, so they're declared locally.
export interface SerialPortLike {
  open(options: { baudRate: number }): Promise<void>;
  close(): Promise<void>;
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
  /** Absent on some polyfills; treated as "unknown vendor" when missing. */
  getInfo?(): { usbVendorId?: number; usbProductId?: number };
  addEventListener(type: "disconnect", listener: () => void): void;
  removeEventListener(type: "disconnect", listener: () => void): void;
}

export interface SerialLike {
  requestPort(options?: {
    filters?: Array<{ usbVendorId?: number }>;
  }): Promise<SerialPortLike>;
  /** Ports the user has already granted this origin access to, no picker needed. */
  getPorts(): Promise<SerialPortLike[]>;
}

function getSerial(): SerialLike {
  const nav = navigator as Navigator & { serial?: SerialLike };
  if (!nav.serial) {
    throw new Error("Web Serial API is not available in this browser");
  }
  return nav.serial;
}

/** True in any environment where `navigator.serial` could plausibly exist. */
export function hasSerialApi(): boolean {
  return typeof navigator !== "undefined" && "serial" in navigator;
}

const BAUD_RATE = 115200;

export class WebSerialTransport implements SerialTransport {
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;

  private constructor(private readonly port: SerialPortLike) {}

  /** Opens a brand-new picker-granted port; only callable from a user gesture. */
  static async requestAndOpen(): Promise<WebSerialTransport> {
    const serial = getSerial();
    const port = await serial.requestPort({
      filters: [{ usbVendorId: ESPRESSIF_USB_VENDOR_ID }],
    });
    return WebSerialTransport.open(port);
  }

  /**
   * Wraps `port`, opening it first if it isn't already. Idempotent: a port
   * that's already open (readable/writable non-null, e.g. reused across a
   * reconnect) is wrapped as-is rather than calling `open()` again, which
   * throws InvalidStateError on an already-open port.
   */
  static async open(port: SerialPortLike): Promise<WebSerialTransport> {
    if (port.readable === null || port.writable === null) {
      await port.open({ baudRate: BAUD_RATE });
    }
    const transport = new WebSerialTransport(port);
    if (!port.readable || !port.writable) {
      throw new Error("serial port opened without readable/writable streams");
    }
    transport.reader = port.readable.getReader();
    transport.writer = port.writable.getWriter();
    return transport;
  }

  /**
   * Reconnects without a picker, per docs/... the requirement that a device
   * page returning from another route re-attaches automatically. Returns
   * null (not a picker) when zero or several previously-granted ports match
   * the vendor id: with several, guessing which one is the flashcard device
   * would be worse than just showing the Connect button.
   */
  static async autoConnect(): Promise<WebSerialTransport | null> {
    if (!hasSerialApi()) return null;
    const serial = getSerial();
    const ports = await serial.getPorts();
    const matches = ports.filter((p) => p.getInfo?.().usbVendorId === ESPRESSIF_USB_VENDOR_ID);
    if (matches.length !== 1) return null;
    return WebSerialTransport.open(matches[0]);
  }

  /** Fires when the OS reports the device physically gone (unplug, reboot into a different mode, ...). */
  onDisconnect(cb: () => void): () => void {
    const handler = () => cb();
    this.port.addEventListener("disconnect", handler);
    return () => this.port.removeEventListener("disconnect", handler);
  }

  async write(data: Uint8Array): Promise<void> {
    if (!this.writer) throw new Error("transport is closed");
    await this.writer.write(data);
  }

  async read(): Promise<Uint8Array | null> {
    if (!this.reader) throw new Error("transport is closed");
    const { value, done } = await this.reader.read();
    if (done) return null;
    return value ?? new Uint8Array(0);
  }

  async close(): Promise<void> {
    await this.reader?.cancel().catch(() => {});
    try {
      this.reader?.releaseLock();
    } catch {
      // Already released, e.g. by a disconnect event racing this close.
    }
    try {
      this.writer?.releaseLock();
    } catch {
      // Same as above.
    }
    this.reader = null;
    this.writer = null;
    await this.port.close().catch(() => {});
  }
}
