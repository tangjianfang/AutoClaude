// hooks/autoclaude-loop.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

function withTmp(fn) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ac-'));
  const prev = process.env.AUTOCLAUDE_DIR;
  process.env.AUTOCLAUDE_DIR = dir;
  for (const m of ['./lib/paths.js','./lib/config.js',
                   './lib/state.js','./lib/decide.js',
                   './autoclaude-loop.js']) {
    try { delete require.cache[require.resolve(m)]; } catch {}
  }
  try { return fn(dir); }
  finally {
    if (prev === undefined) delete process.env.AUTOCLAUDE_DIR;
    else process.env.AUTOCLAUDE_DIR = prev;
    fs.rmSync(dir, { recursive: true, force: true });
  }
}
function writeCfg(dir, o) {
  fs.writeFileSync(path.join(dir, 'loop-config.json'), JSON.stringify(o));
}

test('disabled -> empty stdout', () => {
  withTmp((dir) => {
    writeCfg(dir, { enabled: false });
    const { runHook } = require('./autoclaude-loop.js');
    const r = runHook({ session_id: 's1', cwd: 'C:\\p', last_assistant_message: 'x' });
    assert.strictEqual(r.stdout, '');
  });
});

test('enabled below max -> block JSON and state bumped', () => {
  withTmp((dir) => {
    writeCfg(dir, { enabled: true, maxAutoTurns: 5, continuePrompt: 'GO' });
    const { runHook } = require('./autoclaude-loop.js');
    const r = runHook({ session_id: 's1', cwd: 'C:\\p', last_assistant_message: 'x' });
    const out = JSON.parse(r.stdout);
    assert.strictEqual(out.decision, 'block');
    assert.strictEqual(out.reason, 'GO');
    const st = JSON.parse(fs.readFileSync(path.join(dir, 'loop-state.json'), 'utf8'));
    assert.strictEqual(st.sessions.s1.autoTurns, 1);
  });
});

test('done marker -> empty stdout and state reset', () => {
  withTmp((dir) => {
    writeCfg(dir, { enabled: true, doneMarker: '[[ALL_DONE]]' });
    fs.writeFileSync(path.join(dir, 'loop-state.json'),
      JSON.stringify({ sessions: { s1: { autoTurns: 4 } } }));
    const { runHook } = require('./autoclaude-loop.js');
    const r = runHook({ session_id: 's1', cwd: 'C:\\p', last_assistant_message: 'ok [[ALL_DONE]]' });
    assert.strictEqual(r.stdout, '');
    const st = JSON.parse(fs.readFileSync(path.join(dir, 'loop-state.json'), 'utf8'));
    assert.strictEqual(st.sessions.s1, undefined);
  });
});

test('stop flag present -> empty stdout and flag deleted', () => {
  withTmp((dir) => {
    writeCfg(dir, { enabled: true });
    const flag = path.join(dir, 'stop-flag');
    fs.writeFileSync(flag, '');
    const { runHook } = require('./autoclaude-loop.js');
    const r = runHook({ session_id: 's1', cwd: 'C:\\p', last_assistant_message: 'x' });
    assert.strictEqual(r.stdout, '');
    assert.strictEqual(fs.existsSync(flag), false);
  });
});

test('malformed config -> fail-open empty stdout', () => {
  withTmp((dir) => {
    fs.writeFileSync(path.join(dir, 'loop-config.json'), '{ bad');
    const { runHook } = require('./autoclaude-loop.js');
    const r = runHook({ session_id: 's1', cwd: 'C:\\p', last_assistant_message: 'x' });
    assert.strictEqual(r.stdout, '');
  });
});
