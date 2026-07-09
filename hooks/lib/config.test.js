// hooks/lib/config.test.js
const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

function withTmpAcDir(fn) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ac-'));
  const prev = process.env.AUTOCLAUDE_DIR;
  process.env.AUTOCLAUDE_DIR = dir;
  // 清缓存，确保 paths/config 重新读环境变量
  delete require.cache[require.resolve('./paths.js')];
  delete require.cache[require.resolve('./config.js')];
  try { return fn(dir); }
  finally {
    if (prev === undefined) delete process.env.AUTOCLAUDE_DIR;
    else process.env.AUTOCLAUDE_DIR = prev;
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

test('loadConfig returns defaults when file missing', () => {
  withTmpAcDir(() => {
    const { loadConfig, DEFAULT_CONFIG } = require('./config.js');
    const cfg = loadConfig();
    assert.strictEqual(cfg.enabled, false);
    assert.strictEqual(cfg.maxAutoTurns, 10);
    assert.strictEqual(cfg.doneMarker, DEFAULT_CONFIG.doneMarker);
  });
});

test('loadConfig merges partial file over defaults', () => {
  withTmpAcDir((dir) => {
    fs.writeFileSync(path.join(dir, 'loop-config.json'),
      JSON.stringify({ enabled: true, maxAutoTurns: 3 }));
    const { loadConfig } = require('./config.js');
    const cfg = loadConfig();
    assert.strictEqual(cfg.enabled, true);
    assert.strictEqual(cfg.maxAutoTurns, 3);
    // 未提供的字段回落到默认
    assert.strictEqual(cfg.doneMarker, '[[ALL_DONE]]');
  });
});

test('loadConfig falls back to defaults on malformed json', () => {
  withTmpAcDir((dir) => {
    fs.writeFileSync(path.join(dir, 'loop-config.json'), '{ not json');
    const { loadConfig } = require('./config.js');
    assert.strictEqual(loadConfig().enabled, false);
  });
});
