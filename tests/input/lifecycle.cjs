// SPDX-License-Identifier: GPL-3.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
class Events {
  constructor() { this.listeners = new Map(); this.hidden = false; this.disabled = false; }
  addEventListener(name, callback) {
    if (!this.listeners.has(name)) this.listeners.set(name, []);
    this.listeners.get(name).push(callback);
  }
  async emit(name, event = {}) {
    for (const callback of this.listeners.get(name) || []) await callback(event);
  }
}
const ids = Object.fromEntries(['controller-controls', 'connect-controller', 'disconnect-controller',
  'controller-info', 'video', 'stop'].map(id => [id, new Events()]));
const doc = new Events(), root = new Events(), intervals = new Map(), requests = [];
let focused = true, now = 1000, failState = false, gamepads = [];
let delayedStatus;
doc.getElementById = id => ids[id];
doc.hasFocus = () => focused;
const surface = ids['controller-controls'];
surface.tagName = 'SECTION';
surface.contains = target => target === surface;
surface.focus = () => { focused = true; };
root.document = doc;
root.navigator = {getGamepads: () => gamepads};
root.fetch = async (path, options = {}) => {
  requests.push({path, ...options});
  if (path === '/input/status') return new Promise(resolve => { delayedStatus = resolve; });
  return {
    ok: !(path === '/input/state' && failState),
    json: async () => path === '/input/connect' ? {token: 'a'.repeat(32)} : {message: 'Rejected'}
  };
};
const sandbox = {
  window: root, AbortController, Date: {now: () => now},
  setTimeout: () => 1, clearTimeout: () => {},
  setInterval: (callback, milliseconds) => intervals.set(milliseconds, callback)
};
vm.runInNewContext(fs.readFileSync(require('node:path').join(__dirname,
  '../../payload/web/controller.js'), 'utf8'), sandbox);
const stateRequests = () => requests.filter(request => request.path === '/input/state');
const latestState = () => stateRequests().at(-1).body.split(',').map(Number);
const tick = async () => { now += 120; await intervals.get(16)(); };
const key = async code => doc.emit('keydown', {
  code, target: surface, preventDefault() {}
});
(async () => {
  await tick();
  assert.equal(stateRequests().length, 0, 'no controller or input before explicit connection');
  await ids['connect-controller'].emit('click');
  assert.equal(ids['connect-controller'].disabled, true);
  // A poll started before Connect must not expire the newly acquired session.
  delayedStatus({ok: true, json: async () => ({supported: true, connected: false})});
  await new Promise(setImmediate);
  await key('KeyW'); await key('KeyX'); await tick();
  assert.equal(latestState()[1], 0x4000);
  assert.equal(latestState()[3], 0);
  assert.equal(stateRequests().at(-1).headers['X-PBS-Controller'], 'a'.repeat(32));
  focused = false; await root.emit('blur'); await tick();
  assert.deepEqual(latestState().slice(1), [0, 128, 128, 128, 128, 0, 0]);
  focused = true; await key('KeyR'); doc.hidden = true;
  await doc.emit('visibilitychange'); await tick();
  assert.deepEqual(latestState().slice(1), [0, 128, 128, 128, 128, 0, 0]);
  doc.hidden = false;
  gamepads = [{connected: true, mapping: 'standard', buttons: [{pressed: true}], axes: []}];
  await tick(); assert.equal(latestState()[1], 0x4000);
  gamepads = []; await root.emit('gamepaddisconnected'); await tick();
  assert.equal(latestState()[1], 0);
  await ids['disconnect-controller'].emit('click');
  const before = stateRequests().length;
  await tick(); assert.equal(stateRequests().length, before);
  assert.equal(ids['connect-controller'].disabled, false);
  await ids['connect-controller'].emit('click');
  failState = true; await tick();
  assert.equal(ids['connect-controller'].disabled, false, 'failed input disconnects without retry');
  failState = false; await ids['connect-controller'].emit('click');
  await root.emit('pagehide');
  assert.equal(requests.at(-1).path, '/input/disconnect');
  assert.equal(requests.at(-1).keepalive, true);
  const connects = requests.filter(request => request.path === '/input/connect').length;
  await tick();
  assert.equal(requests.filter(request => request.path === '/input/connect').length, connects);
  console.log('Controller browser lifecycle: explicit connection, stale status, focus loss, errors and page exit verified');
})().catch(error => { console.error(error); process.exitCode = 1; });
