// SPDX-License-Identifier: GPL-3.0-or-later
// Avcap sends PCM separately from H.264. Expose that channel to Video.js
// instead of the silent native video's volume (which is read-only on iOS).
export function pcmVolumeFeature(audio) {
  return {
    name: 'volume',
    state: () => ({
      ...audio.snapshot(),
      setVolume(value) {
        const volume = Math.max(0, Math.min(1, Number(value) || 0));
        audio.set({volume, ...(volume > 0 ? {muted: false} : {})});
        return volume;
      },
      setMuted(muted) {
        const volume = audio.snapshot().volume;
        audio.set({muted: Boolean(muted), ...(!muted && !volume ? {volume: 0.25} : {})});
        return Boolean(muted);
      }
    }),
    attach({signal, set}) {
      const sync = () => set(audio.snapshot());
      audio.subscribe(sync, signal);
      sync();
    }
  };
}

export function intentPlaybackFeature(nativeFeature, playback) {
  return {
    ...nativeFeature,
    state(context) {
      const state = nativeFeature.state(context);
      return {
        ...state,
        play() { playback.resume(); return state.play(); },
        pause() { playback.pause(); return state.pause(); }
      };
    }
  };
}
