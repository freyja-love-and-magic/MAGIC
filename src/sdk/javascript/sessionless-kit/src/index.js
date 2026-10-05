import { registerFileStore } from './identity.js';
import { fileStore } from './stores-node.js';

registerFileStore(fileStore);

export { Identity, verifyEnvelope, signTimestamped, verifyTimestamped } from './identity.js';
export { verify } from './verify.js';
export { memoryStore, localStorageStore, customStore } from './stores.js';
export { fileStore, defaultKeyDir } from './stores-node.js';
