import { secp256k1 } from 'ethereum-cryptography/secp256k1';
import { keccak256 } from 'ethereum-cryptography/keccak.js';
import { utf8ToBytes } from 'ethereum-cryptography/utils.js';

// Standalone, stateless verification. Wire-compatible with sessionless-node:
// keccak256(utf8(message)), secp256k1, 64-byte compact hex signature.
//
// Returns false for anything that isn't a valid signature, including
// malformed hex, wrong lengths, and bad public keys. It only throws if the
// arguments aren't strings, since that's a programming error rather than a
// forged signature.
export const verify = (signature, message, pubKey) => {
  for (const [name, value] of [['signature', signature], ['message', message], ['pubKey', pubKey]]) {
    if (typeof value !== 'string') {
      throw new TypeError(`verify: ${name} must be a string, got ${typeof value}`);
    }
  }
  try {
    return secp256k1.verify(signature, keccak256(utf8ToBytes(message)), pubKey);
  } catch {
    return false;
  }
};
