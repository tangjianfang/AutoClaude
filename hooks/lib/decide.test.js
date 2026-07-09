// hooks/lib/decide.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const { decide } = require('./decide.js');
const { DEFAULT_CONFIG } = require('./config.js');

const baseInput = {
  sessionId: 's1', cwd: 'C:\\proj',
  lastAssistantMessage: 'done thinking', stopFlagPresent: false,
};
function cfg(over = {}) { return { ...DEFAULT_CONFIG, enabled: true, ...over }; }
function state(turns = 0) { return { sessions: turns ? { s1: { autoTurns: turns } } : {} }; }

test('disabled config always allows', () => {
  const d = decide(baseInput, cfg({ enabled: false }), state());
  assert.strictEqual(d.action, 'allow');
});

test('stop flag present -> allow and clear', () => {
  const d = decide({ ...baseInput, stopFlagPresent: true }, cfg(), state());
  assert.strictEqual(d.action, 'allow');
  assert.strictEqual(d.clearStopFlags, true);
});

test('done marker in last message -> allow and reset', () => {
  const d = decide({ ...baseInput, lastAssistantMessage: 'all done [[ALL_DONE]]' }, cfg(), state(3));
  assert.strictEqual(d.action, 'allow');
  assert.strictEqual(d.resetSession, true);
});

test('at max turns -> allow and reset', () => {
  const d = decide(baseInput, cfg({ maxAutoTurns: 3 }), state(3));
  assert.strictEqual(d.action, 'allow');
  assert.strictEqual(d.resetSession, true);
});

test('below max, enabled, no markers -> block with prompt and bump', () => {
  const d = decide(baseInput, cfg({ maxAutoTurns: 5 }), state(2));
  assert.strictEqual(d.action, 'block');
  assert.strictEqual(d.reason, DEFAULT_CONFIG.continuePrompt);
  assert.strictEqual(d.bumpSession, true);
});

test('project scope mismatch -> allow', () => {
  const d = decide(baseInput, cfg({ scope: 'project', projectFilter: 'D:\\other' }), state());
  assert.strictEqual(d.action, 'allow');
});

test('project scope match (case-insensitive prefix) -> block', () => {
  const d = decide({ ...baseInput, cwd: 'C:\\Proj\\sub' },
                   cfg({ scope: 'project', projectFilter: 'c:\\proj' }), state());
  assert.strictEqual(d.action, 'block');
});
