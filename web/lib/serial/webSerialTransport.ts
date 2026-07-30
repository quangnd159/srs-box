// Browser-only adapter: wraps the Web Serial API's SerialPort as the
// SerialTransport interface lib/serial/protocol.ts expects. Not unit
// tested here (WebSerial doesn't exist outside a browser); protocol.ts
// itself is tested against a mock in test/serialProtocol.test.ts, per
// CLAUDE.md's "you cannot test WebSerial against real hardware" note.
import type { SerialTransport } from "./transport";

// Espressif's USB vendor id (see CLAUDE.md and tools/devctl.py).
export const ESPRESSIF_USB_VENDOR_ID = 0x303a;

// Minimal Web Serial API surface this module needs. Not every browser's
// lib.dom.d.ts ships these types yet, so they're declared locally.
interface SerialPortLike {
  open(options: { baudRate: number }): Promise<void>;
  close(): Promise<void>;
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
}

interface SerialLike {
  requestPort(options?: {
    filters?: Array<{ usbVendorId?: number }>;
  }): Promise<SerialPortLike>;
}

function getSerial(): SerialLike {
  const nav = navigator as Navigator & { serial?: SerialLike };
  if (!nav.serial) {
    throw new Error("Web Serial API is not available in this browser");
  }
  return nav.serial;
}

const BAUD_RATE = 115200;

export class WebSerialTransport implements SerialTransport {
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;

  private constructor(private readonly port: SerialPortLike) {}

  static async requestAndOpen(): Promise<WebSerialTransport> {
    const serial = getSerial();
    const port = await serial.requestPort({
      filters: [{ usbVendorId: ESPRESSIF_USB_VENDOR_ID }],
    });
    await port.open({ baudRate: BAUD_RATE });
    const transport = new WebSerialTransport(port);
    if (!port.readable || !port.writable) {
      throw new Error("serial port opened without readable/writable streams");
    }
    transport.reader = port.readable.getReader();
    transport.writer = port.writable.getWriter();
    return transport;
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
    this.reader?.releaseLock();
    this.writer?.releaseLock();
    await this.port.close().catch(() => {});
  }
}
