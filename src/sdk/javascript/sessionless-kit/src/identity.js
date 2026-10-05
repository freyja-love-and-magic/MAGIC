import { secp256k1 } from 'ethereum-cryptography/secp256k1';
import { keccak256 } from 'ethereum-cryptography/keccak.js';
import { bytesToHex, utf8ToBytes } from 'ethereum-cryptography/utils.js';
import { memoryStore, customStore } from './stores.js';
import { verify } from './verify.js';

const PRIVATE_KEY_RE = /^[0-9a-fA-F]{64}$/;

const derivePubKey = (privateKey) => bytesToHex(secp256k1.getPublicKey(privateKey));

const validateKeys = (keys, where) => {
  if (!keys || typeof keys.privateKey !== 'string' || !PRIVATE_KEY_RE.test(keys.privateKey)) {
    throw new Error(`${where}: expected { privateKey } as 64 hex characters`);
  }
  const pubKey = derivePubKey(keys.privateKey);
  if (keys.pubKey && keys.pubKey !== pubKey) {
    throw new Error(`${where}: stored pubKey does not match its privateKey; the key file is corrupt or has been tampered with`);
  }
  return { privateKey: keys.privateKey.toLowerCase(), pubKey };
};

// File storage is Node-only, so this module doesn't import it: the Node entry (index.js) registers it. A browser
// bundle never includes node:fs, and asking a browser for file storage gives a clear error instead.
let fileStoreFactory = null;
export const registerFileStore = (factory) => { fileStoreFactory = factory; };

const resolveStore = async ({ store, saveKeys, getKeys, name, path }) => {
  if (store) return store;
  if (saveKeys || getKeys) return customStore({ saveKeys, getKeys });
  if (!fileStoreFactory) {
    throw new Error('File storage is only available in Node. In a browser pass { store: localStorageStore() } (or any { load, save } store).');
  }
  return fileStoreFactory({ name, path });
};

// A Sessionless identity: one secp256k1 keypair plus the store it lives in.
// Holds no global state, so any number of identities can coexist in a process
// (unlike sessionless-node, whose getKeys is module-global).
export class Identity {
  #privateKey;

  constructor({ privateKey, pubKey }) {
    this.#privateKey = privateKey;
    this.pubKey = pubKey;
    Object.freeze(this);
  }

  // Load the identity from storage, creating and saving a new one if none exists.
  // Default storage is ~/.sessionless/<name>.json (0600).
  //   Identity.loadOrCreate({ name: 'caster' })
  //   Identity.loadOrCreate({ path: './keys.json' })
  //   Identity.loadOrCreate({ store: memoryStore() })
  //   Identity.loadOrCreate({ saveKeys, getKeys })   // sessionless-node style
  static async loadOrCreate(options = {}) {
    const store = await resolveStore(options);
    const existing = await store.load();
    if (existing) return new Identity(validateKeys(existing, 'Stored keys'));
    return Identity.#create(store);
  }

  // Load an identity that must already exist. Fails clearly if it doesn't.
  static async load(options = {}) {
    const store = await resolveStore(options);
    const existing = await store.load();
    if (!existing) {
      throw new Error(`No keys found${store.path ? ` at ${store.path}` : ''}. Create them first with Identity.create() or Identity.loadOrCreate().`);
    }
    return new Identity(validateKeys(existing, 'Stored keys'));
  }

  // Create a brand-new identity and save it. Fails if keys already exist
  // in the store, so a key can never be silently replaced.
  static async create(options = {}) {
    const store = await resolveStore(options);
    if (await store.load()) {
      throw new Error(`Keys already exist${store.path ? ` at ${store.path}` : ''}; refusing to overwrite. Use Identity.load() or delete them deliberately.`);
    }
    return Identity.#create(store);
  }

  // Create an in-memory identity that is never saved. Handy for tests and throwaway callers.
  static ephemeral() {
    return Identity.#create(memoryStore());
  }

  // Bring an existing private key (hex) into a store, e.g. a recovery key or one exported elsewhere.
  // Refuses to overwrite different keys already in the store.
  static async import(privateKey, options = {}) {
    const keys = validateKeys({ privateKey }, 'import');
    const store = await resolveStore(options);
    const existing = await store.load();
    if (existing && validateKeys(existing, 'Stored keys').privateKey !== keys.privateKey) {
      throw new Error(`Different keys already exist${store.path ? ` at ${store.path}` : ''}; refusing to overwrite.`);
    }
    if (!existing) await store.save(keys);
    return new Identity(keys);
  }

  static async #create(store) {
    const privateKey = bytesToHex(secp256k1.utils.randomPrivateKey());
    const keys = { privateKey, pubKey: derivePubKey(privateKey) };
    await store.save(keys);
    return new Identity(keys);
  }

  // Sign a message string. Returns a 128-char compact hex signature.
  // Async so it also satisfies the signer interface used by magic-gateway-js and sessionless-node callers.
  async sign(message) {
    if (typeof message !== 'string') {
      throw new TypeError(`sign: message must be a string, got ${typeof message}`);
    }
    return secp256k1.sign(keccak256(utf8ToBytes(message)), this.#privateKey).toCompactHex();
  }

  // Verify a signature against this identity's public key.
  verify(signature, message) {
    return verify(signature, message, this.pubKey);
  }

  // Sign a message and bundle everything a verifier needs. Pairs with verifyEnvelope().
  async signEnvelope(message) {
    return { message, signature: await this.sign(message), pubKey: this.pubKey };
  }

  // The private key as hex, for backup or moving to another device. Treat it like a password.
  exportPrivateKey() {
    return this.#privateKey;
  }

  // Shape sessionless-node callers expect, for code that calls getKeys().
  async getKeys() {
    return { privateKey: this.#privateKey, pubKey: this.pubKey };
  }

  toJSON() {
    return { pubKey: this.pubKey };
  }

  [Symbol.for('nodejs.util.inspect.custom')]() {
    return `Identity { pubKey: '${this.pubKey}' }`;
  }
}

// Verify a bundle from signEnvelope(). Optionally pin the expected signer and/or message,
// which you should do whenever you know what you expect:
//   verifyEnvelope(env, { pubKey: knownKey })
// An envelope's own pubKey only proves someone holding *that* key signed it, not who they are.
export const verifyEnvelope = (envelope, { pubKey, message } = {}) => {
  if (!envelope || typeof envelope.signature !== 'string' || typeof envelope.message !== 'string' || typeof envelope.pubKey !== 'string') {
    return false;
  }
  if (pubKey !== undefined && envelope.pubKey !== pubKey) return false;
  if (message !== undefined && envelope.message !== message) return false;
  return verify(envelope.signature, envelope.message, envelope.pubKey);
};

// The sessionless convention for requests is to sign `timestamp + ...fields`.
// These helpers do that and enforce freshness so replayed requests can be rejected.
export const signTimestamped = async (identity, ...fields) => {
  const timestamp = Date.now().toString();
  const message = timestamp + fields.join('');
  return { timestamp, message, signature: await identity.sign(message), pubKey: identity.pubKey };
};

export const verifyTimestamped = ({ timestamp, signature, pubKey }, fields, { maxAgeMs = 60_000, now = Date.now() } = {}) => {
  const ts = Number(timestamp);
  if (!Number.isFinite(ts) || Math.abs(now - ts) > maxAgeMs) return false;
  return verify(signature, String(timestamp) + [].concat(fields).join(''), pubKey);
};
