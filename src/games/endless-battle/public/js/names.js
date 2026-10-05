// A stable, friendly display name for a viewer, derived from their casterUUID. Viewers never type a name,
// so nothing a viewer controls ever appears on a live stream, and the name can't be used to look anyone up.
const ADJECTIVES = ['Brave', 'Swift', 'Sly', 'Jolly', 'Mighty', 'Clever', 'Fuzzy', 'Lucky', 'Gentle', 'Wild', 'Sunny', 'Plucky', 'Quiet', 'Bold', 'Merry', 'Noble'];
const ANIMALS = ['Otter', 'Falcon', 'Badger', 'Fox', 'Heron', 'Newt', 'Lynx', 'Moth', 'Wolf', 'Hare', 'Crane', 'Toad', 'Bear', 'Finch', 'Mole', 'Stag'];

// FNV-1a: tiny, deterministic, no dependencies.
const hash = (s) => {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 0x01000193) >>> 0; }
  return h >>> 0;
};

export const viewerName = (casterUUID) => {
  const h = hash(String(casterUUID));
  return `${ADJECTIVES[h & 15]} ${ANIMALS[(h >>> 4) & 15]}`;
};
