// hooks/lib/broadcast.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

function withTmp(fn) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ac-'));
  const prev = process.env.AUTOCLAUDE_DIR;
  process.env.AUTOCLAUDE_DIR = dir;
  for (const m of ['./paths.js','./config.js','./state.js','./broadcast.js']) {
    try { delete require.cache[require.resolve(m)]; } catch {}
  }
  try { return fn(dir); }
  finally {
    if (prev === undefined) delete process.env.AUTOCLAUDE_DIR;
    else process.env.AUTOCLAUDE_DIR = prev;
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

test('buildFrame reflects config enabled and session turns', () => {
  withTmp((dir) => {
    fs.writeFileSync(path.join(dir, 'loop-config.json'), JSON.stringify({ enabled: true }));
    fs.writeFileSync(path.join(dir, 'loop-state.json'),
      JSON.stringify({ sessions: { s1: { autoTurns: 4 } } }));
    const { buildFrame } = require('./broadcast.js');
    const line = buildFrame({ sessionId: 's1', cwd: 'C:\\p', transcriptPath: 't.jsonl' });
    assert.ok(line.endsWith('\n'));
    const obj = JSON.parse(line);
    assert.strictEqual(obj.sessionId, 's1');
    assert.strictEqual(obj.loopEnabled, true);
    assert.strictEqual(obj.autoTurns, 4);
  });
});

test('sendFrame does not throw when pipe absent', () => {
  withTmp(() => {
    const { sendFrame } = require('./broadcast.js');
    assert.doesNotThrow(() => sendFrame('{"x":1}\n'));
  });
});
