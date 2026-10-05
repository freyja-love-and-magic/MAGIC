import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

// Node-only key storage.
export const defaultKeyDir = () => process.env.SESSIONLESS_HOME || path.join(os.homedir(), '.sessionless');

const NAME_RE = /^[A-Za-z0-9._-]+$/;

// Keys in a JSON file, owner-only (dir 0700, file 0600), written atomically.
// Pass { path } for an explicit file, or { name } for ~/.sessionless/<name>.json
// (override the directory with SESSIONLESS_HOME).
export const fileStore = ({ path: filePath, name = 'default' } = {}) => {
  if (!filePath) {
    if (!NAME_RE.test(name) || name === '.' || name === '..') {
      throw new Error(`Invalid identity name "${name}": use letters, numbers, dot, dash, underscore`);
    }
    filePath = path.join(defaultKeyDir(), `${name}.json`);
  }
  filePath = path.resolve(filePath);

  return {
    path: filePath,
    load() {
      let raw;
      try {
        raw = fs.readFileSync(filePath, 'utf-8');
      } catch (err) {
        if (err.code === 'ENOENT') return null;
        throw err;
      }
      try {
        return JSON.parse(raw);
      } catch (err) {
        throw new Error(`Key file ${filePath} is not valid JSON (${err.message}). Refusing to continue so it isn't overwritten.`);
      }
    },
    save(keys) {
      fs.mkdirSync(path.dirname(filePath), { recursive: true, mode: 0o700 });
      const tmp = `${filePath}.${process.pid}.${Date.now()}.tmp`;
      fs.writeFileSync(tmp, JSON.stringify(keys, null, 2), { mode: 0o600 });
      fs.renameSync(tmp, filePath);
    }
  };
};
