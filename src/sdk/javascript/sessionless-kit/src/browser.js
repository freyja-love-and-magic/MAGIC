// Browser entry: everything except Node file storage. Use localStorageStore (or any {load, save} store).
//   import { Identity, localStorageStore, verify } from 'sessionless-kit/browser';
//   const me = await Identity.loadOrCreate({ store: localStorageStore({ key: 'my-app-keys' }) });
export { Identity, verifyEnvelope, signTimestamped, verifyTimestamped } from './identity.js';
export { verify } from './verify.js';
export { memoryStore, localStorageStore, customStore } from './stores.js';
