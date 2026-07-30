// A mock duplex byte stream for testing lib/serial/protocol.ts without
// WebSerial or real hardware. Two Pipes, one per direction, connected to a
// pair of SerialTransport-shaped ends: the "host" end (driven by
// DeviceClient, the code under test) and the "device" end (driven by a
// small fake-firmware loop in each test).
import type { SerialTransport } from "../lib/serial/transport";

class Pipe {
  private queue: Uint8Array[] = [];
  private waiters: Array<(v: Uint8Array | null) => void> = [];
  private ended = false;

  push(chunk: Uint8Array): void {
    if (this.waiters.length > 0) {
      this.waiters.shift()!(chunk);
    } else {
      this.queue.push(chunk);
    }
  }

  end(): void {
    this.ended = true;
    while (this.waiters.length > 0) this.waiters.shift()!(null);
  }

  read(): Promise<Uint8Array | null> {
    if (this.queue.length > 0) return Promise.resolve(this.queue.shift()!);
    if (this.ended) return Promise.resolve(null);
    return new Promise((resolve) => this.waiters.push(resolve));
  }
}

export interface DuplexPair {
  host: SerialTransport;
  device: SerialTransport;
  close(): void;
}

/** Creates a connected pair: bytes written on one side are readable on the other. */
export function makeDuplexPair(): DuplexPair {
  const toDevice = new Pipe();
  const toHost = new Pipe();

  return {
    host: {
      write: (data) => {
        toDevice.push(data);
        return Promise.resolve();
      },
      read: () => toHost.read(),
    },
    device: {
      write: (data) => {
        toHost.push(data);
        return Promise.resolve();
      },
      read: () => toDevice.read(),
    },
    close: () => {
      toDevice.end();
      toHost.end();
    },
  };
}
