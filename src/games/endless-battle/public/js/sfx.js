// Small synthesized sound effects: oscillators and noise, no audio files. Nothing fancy on purpose.
// Browsers (not OBS) keep audio locked until the page is clicked once, so `unlock()` is called on a click.
export const createSfx = ({ muted = false, volume = 0.9 } = {}) => {
  let ctx = null;
  let master = null;

  const ensure = () => {
    if (ctx || muted) return ctx;
    const AC = globalThis.AudioContext || globalThis.webkitAudioContext;
    if (!AC) return null;
    ctx = new AC();
    master = ctx.createGain();
    master.gain.value = volume;
    master.connect(ctx.destination);
    return ctx;
  };

  // Quick attack, then a two-stage decay: it keeps most of its level for the first third, then fades out. A plain
  // exponential from the peak was inaudible after a quarter of the intended length.
  const env = (g, t, attack, dur, peak) => {
    g.gain.setValueAtTime(0.0001, t);
    g.gain.exponentialRampToValueAtTime(peak, t + attack);
    g.gain.linearRampToValueAtTime(peak * 0.45, t + dur * 0.4);
    g.gain.exponentialRampToValueAtTime(0.0001, t + dur);
  };

  // A tone that can glide from one pitch to another.
  const tone = (freq, dur, { type = 'square', vol = 0.18, to, delay = 0 } = {}) => {
    const c = ensure(); if (!c) return;
    const t = c.currentTime + delay;
    const o = c.createOscillator(), g = c.createGain();
    o.type = type;
    o.frequency.setValueAtTime(freq, t);
    if (to) o.frequency.exponentialRampToValueAtTime(to, t + dur);
    env(g, t, 0.01, dur, vol);
    o.connect(g); g.connect(master);
    o.start(t); o.stop(t + dur + 0.05);
  };

  // A burst of filtered noise (impacts, fire, rumble).
  const noise = (dur, { vol = 0.25, from = 3000, to = 400, delay = 0, type = 'lowpass' } = {}) => {
    const c = ensure(); if (!c) return;
    const t = c.currentTime + delay;
    const buf = c.createBuffer(1, Math.max(1, Math.floor(c.sampleRate * dur)), c.sampleRate);
    const data = buf.getChannelData(0);
    for (let i = 0; i < data.length; i++) data[i] = Math.random() * 2 - 1;
    const src = c.createBufferSource(); src.buffer = buf;
    const f = c.createBiquadFilter(); f.type = type;
    f.frequency.setValueAtTime(from, t); f.frequency.exponentialRampToValueAtTime(to, t + dur);
    const g = c.createGain(); env(g, t, 0.005, dur, vol);
    src.connect(f); f.connect(g); g.connect(master);
    src.start(t);
  };

  const arpeggio = (notes, step, opts) => notes.forEach((n, i) => tone(n, step * 2.2, { ...opts, delay: i * step }));

  return {
    get running() { return !!ctx && ctx.state === 'running'; },
    get available() { return !muted; },
    unlock() { const c = ensure(); if (c && c.state !== 'running') return c.resume(); },
    swing() { noise(0.14, { vol: 0.5, from: 5000, to: 800, type: 'bandpass' }); },
    hit() { noise(0.16, { vol: 0.5, from: 2500, to: 200 }); tone(140, 0.14, { type: 'square', to: 60, vol: 0.3 }); },
    crit() { this.hit(); tone(880, 0.1, { type: 'square', delay: 0.04, vol: 0.2 }); tone(1320, 0.2, { type: 'square', delay: 0.1, vol: 0.18 }); },
    fire() { noise(0.55, { vol: 0.5, from: 1800, to: 250 }); tone(300, 0.4, { type: 'sawtooth', to: 90, vol: 0.2 }); },
    thunder() { noise(0.4, { vol: 0.5, from: 6000, to: 500, type: 'highpass' }); tone(1500, 0.25, { type: 'sawtooth', to: 120, vol: 0.2 }); },
    meteor() { tone(900, 0.5, { type: 'sawtooth', to: 60, vol: 0.25 }); noise(0.9, { vol: 0.7, from: 1200, to: 80, delay: 0.5 }); tone(55, 0.9, { type: 'sine', vol: 0.6, delay: 0.5 }); },
    cast() { arpeggio([523, 659, 784], 0.06, { type: 'triangle', vol: 0.24 }); },
    heal() { arpeggio([659, 784, 988, 1319], 0.07, { type: 'sine', vol: 0.3 }); },
    revive() { arpeggio([392, 523, 659, 784, 1047, 1319], 0.07, { type: 'triangle', vol: 0.32 }); tone(1568, 0.8, { type: 'sine', vol: 0.2, delay: 0.42 }); },
    ko() { tone(330, 0.55, { type: 'square', to: 70, vol: 0.3 }); noise(0.3, { vol: 0.3, from: 900, to: 120 }); },
    haste() { tone(300, 0.4, { type: 'sawtooth', to: 1200, vol: 0.25 }); },
    viewer() { tone(988, 0.09, { type: 'square', vol: 0.34 }); tone(1319, 0.24, { type: 'square', vol: 0.34, delay: 0.09 }); }
  };
};
