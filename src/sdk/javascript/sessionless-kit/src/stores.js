// Stores that work everywhere (Node and browsers). File storage is in stores-node.js so a browser bundle
// never pulls in node:fs.
// A store is any object with:
//   load() -> { privateKey, pubKey } | null     (sync or async)
//   save({ privateKey, pubKey })                (sync or async)
// Keys are never overwritten implicitly: Identity only calls save() for keys it just created or imported.

export const memoryStore = () => {
  let keys = null;
  return {
    load: () => keys,
    save: (k) => { keys = { ...k }; }
  };
};

// Browser storage. Note that localStorage is readable by any script on the
// origin, so only use it for keys you're comfortable exposing to that origin.
export const localStorageStore = ({ key = 'sessionless-keys', storage = globalThis.localStorage } = {}) => {
  if (!storage) throw new Error('localStorageStore: no localStorage available in this environment');
  return {
    load() {
      const raw = storage.getItem(key);
      return raw ? JSON.parse(raw) : null;
    },
    save(keys) {
      storage.setItem(key, JSON.stringify(keys));
    }
  };
};

// Adapt the saveKeys/getKeys pair that sessionless-node uses.
export const customStore = ({ saveKeys, getKeys }) => {
  if (typeof saveKeys !== 'function' || typeof getKeys !== 'function') {
    throw new TypeError('customStore needs both saveKeys and getKeys functions');
  }
  return {
    load: async () => (await getKeys()) || null,
    save: (keys) => saveKeys(keys)
  };
};
