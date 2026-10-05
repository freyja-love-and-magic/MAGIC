// The spells a viewer can cast. Shared by the gateway (which enforces cost and cooldown) and the cast page.
// `cost` is the MP the viewer's signed spell must carry: fount does NOT check it against anything, so the
// gateway does (see src/gateway.js). `effect` is what the battle engine does.
export const SPELLS = {
  arenaFire:   { effect: 'fire',   name: 'Fire',   icon: '🔥', cost: 50,  cooldownMs: 2500,  blurb: 'Scorch the dragon' },
  arenaHeal:   { effect: 'heal',   name: 'Heal',   icon: '💚', cost: 40,  cooldownMs: 4000,  blurb: 'Mend the whole party' },
  arenaRevive: { effect: 'revive', name: 'Revive', icon: '✨', cost: 80,  cooldownMs: 4000,  blurb: 'Raise fallen heroes' },
  arenaHaste:  { effect: 'haste',  name: 'Haste',  icon: '⚡', cost: 60,  cooldownMs: 12000, blurb: 'Speed up the party' },
  arenaMeteor: { effect: 'meteor', name: 'Meteor', icon: '🌠', cost: 200, cooldownMs: 15000, blurb: 'Crush every enemy' }
};
