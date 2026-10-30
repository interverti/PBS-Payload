// PCM volume and playback intent must survive Video.js media reattachments.
const assert = require('node:assert/strict');
const {pathToFileURL} = require('node:url');
const path = require('node:path');
(async () => {
  const {pcmVolumeFeature, intentPlaybackFeature} = await import(pathToFileURL(path.join(__dirname, '../../payload/web/videojs-features.js')));
  let snapshot = {volume: 0.375, muted: true, volumeAvailability: 'available', mutedAvailability: 'available'};
  const listeners = new Set();
  const audio = {
    snapshot: () => ({...snapshot}),
    set(values) { Object.assign(snapshot, values); for (const fn of listeners) fn(); },
    subscribe(fn, signal) { listeners.add(fn); signal.addEventListener('abort', () => listeners.delete(fn), {once: true}); }
  };
  const feature = pcmVolumeFeature(audio);
  const actions = feature.state();
  for (let attempt = 0; attempt < 3; attempt++) {
    const controller = new AbortController();
    let state;
    feature.attach({signal: controller.signal, set: values => {state = values;}});
    assert.equal(listeners.size, 1);
    assert.equal(state.volume, snapshot.volume, 'reattach retains user volume');
    actions.setVolume(0.7);
    assert.equal(state.volume, 0.7);
    assert.equal(state.muted, false, 'volume slider enables the PCM channel');
    actions.setMuted(true);
    assert.equal(state.muted, true);
    assert.equal(state.volume, 0.7, 'mute retains volume');
    actions.setVolume(-4);
    assert.equal(state.volume, 0);
    actions.setMuted(false);
    assert.equal(state.volume, 0.25, 'unmute restores audible volume from zero');
    actions.setVolume(99);
    assert.equal(state.volume, 1);
    actions.setVolume(NaN);
    assert.equal(state.volume, 0);
    audio.set({volumeAvailability: 'unavailable', mutedAvailability: 'unavailable'});
    assert.equal(state.volumeAvailability, 'unavailable');
    controller.abort();
    assert.equal(listeners.size, 0, 'media cleanup removes subscriptions');
  }
  const sequence = [];
  const native = {name: 'playback', attach: () => {}, state: () => ({paused: true,
    play() {sequence.push('native-play'); return Promise.resolve();}, pause() {sequence.push('native-pause');}})};
  const wrapped = intentPlaybackFeature(native, {
    resume() {sequence.push('resume-intent');}, pause() {sequence.push('pause-intent');}
  });
  assert.equal(wrapped.attach, native.attach, 'preserve native event handling');
  const playback = wrapped.state({});
  await playback.play(); playback.pause();
  assert.deepEqual(sequence, ['resume-intent', 'native-play', 'pause-intent', 'native-pause']);
  console.log('PASS: Video.js PCM volume/mute, zero-volume unmute, availability, reattach cleanup and playback intent');
})().catch(error => {console.error(error); process.exitCode = 1;});
