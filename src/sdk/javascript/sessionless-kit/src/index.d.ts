export interface Keys {
  privateKey: string;
  pubKey: string;
}

export interface KeyStore {
  load(): Keys | null | Promise<Keys | null>;
  save(keys: Keys): void | Promise<void>;
  path?: string;
}

export interface StoreOptions {
  /** Identity name; keys live at ~/.sessionless/<name>.json (default "default"). */
  name?: string;
  /** Explicit key file path. */
  path?: string;
  /** Any custom store. */
  store?: KeyStore;
  /** sessionless-node style persistence callbacks (both required together). */
  saveKeys?: (keys: Keys) => void | Promise<void>;
  getKeys?: () => Keys | null | Promise<Keys | null>;
}

export interface Envelope {
  message: string;
  signature: string;
  pubKey: string;
}

export class Identity {
  readonly pubKey: string;
  static loadOrCreate(options?: StoreOptions): Promise<Identity>;
  static load(options?: StoreOptions): Promise<Identity>;
  static create(options?: StoreOptions): Promise<Identity>;
  static ephemeral(): Promise<Identity>;
  static import(privateKey: string, options?: StoreOptions): Promise<Identity>;
  sign(message: string): Promise<string>;
  verify(signature: string, message: string): boolean;
  signEnvelope(message: string): Promise<Envelope>;
  exportPrivateKey(): string;
  getKeys(): Promise<Keys>;
}

export function verify(signature: string, message: string, pubKey: string): boolean;
export function verifyEnvelope(envelope: Envelope, expected?: { pubKey?: string; message?: string }): boolean;

export function signTimestamped(identity: Identity, ...fields: Array<string | number | boolean>): Promise<{ timestamp: string; message: string; signature: string; pubKey: string }>;
export function verifyTimestamped(
  signed: { timestamp: string; signature: string; pubKey: string },
  fields: string | Array<string | number | boolean>,
  options?: { maxAgeMs?: number; now?: number }
): boolean;

export function fileStore(options?: { name?: string; path?: string }): KeyStore;
export function memoryStore(): KeyStore;
export function localStorageStore(options?: { key?: string; storage?: Storage }): KeyStore;
export function customStore(options: { saveKeys: StoreOptions['saveKeys']; getKeys: StoreOptions['getKeys'] }): KeyStore;
export function defaultKeyDir(): string;
