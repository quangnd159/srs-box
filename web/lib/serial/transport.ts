/**
 * A duplex byte stream, abstracting over WebSerial so the protocol client
 * is testable without a browser or real hardware (see
 * test/serialProtocol.test.ts, which drives it with an in-memory mock).
 */
export interface SerialTransport {
  /** Writes raw bytes to the device. */
  write(data: Uint8Array): Promise<void>;
  /** Reads the next available chunk of bytes, or null when the stream has closed. */
  read(): Promise<Uint8Array | null>;
}
