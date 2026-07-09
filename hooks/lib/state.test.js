// hooks/lib/state.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

function withTmp(fn) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ac-'));
  const prev = process.env.AUTOCLAUDE_DIR;
  process.env.AUTOCLAUDE_DIR = dir;
  delete require.cache[require.resolve('./paths.js')];
  delete require.cache[require.resolve('./state.js')];
  try { return fn(dir); }
  finally {
    if (prev === undefined) delete process.env.AUTOCLAUDE_DIR;
    else process.env.AUTOCLAUDE_DIR = prev;
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

test('loadState returns empty sessions when missing', () => {
  withTmp(() => {
    const s = require('./state.js');
    assert.deepStrictEqual(s.loadState(), { sessions: {} });
  });
});

test('bump / get / reset turns roundtrip through save+load', () => {
  withTmp(() => {
    const s = require('./state.js');
    const st = s.loadState();
    s.bumpTurns(st, 'sess1');
    s.bumpTurns(st, 'sess1');
    assert.strictEqual(s.getTurns(st, 'sess1'), 2);
    s.saveState(st);

    delete require.cache[require.resolve('./state.js')];
    const s2 = require('./state.js');
    const reloaded = s2.loadState();
    assert.strictEqual(s2.getTurns(reloaded, 'sess1'), 2);

    s2.resetTurns(reloaded, 'sess1');
    assert.strictEqual(s2.getTurns(reloaded, 'sess1'), 0);
  });
});

test('saveState is atomic (no leftover tmp file)', () => {
  withTmp((dir) => {
    const s = require('./state.js');
    const st = s.loadState();
    s.bumpTurns(st, 'x');
    s.saveState(st);
    const leftovers = fs.readdirSync(dir).filter(f => f.includes('.tmp'));
    assert.deepStrictEqual(leftovers, []);
  });
});
