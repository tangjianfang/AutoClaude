# AutoClaude 自动接力循环 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 Claude Code 在完成一轮任务后，通过 Stop hook 自动注入"继续下一步"提示词循环推进，直到满足停止条件；AutoClaude GUI 实时观测轮次并可随时喊停。

**Architecture:** 三个解耦组件。(1) Node 写的 Stop hook `autoclaude-loop.js` 是执行端，读配置/轮次/停止标志决策是否让 Claude 继续——不依赖 GUI 即可自动接力。(2) `statusline.js` 扩展为传感器，每次状态栏刷新经命名管道广播状态。(3) AutoClaude C++ 起命名管道服务端接收广播，显示 `loop N/M` 并提供 Stop Loop 按钮（写停止标志）。

**Tech Stack:** Node.js（内置 `node --test`，无外部依赖）、C++17 / Win32 / GDI+、命名管道 IPC、nlohmann::json（已 vendored）。

## Global Constraints

- 所有 IPC 与状态文件位于 `%USERPROFILE%\.claude\autoclaude\`（下称 `$AC_DIR`）。
- Hook 全程 **fail-open**：任何异常或读不到文件 → 放行（输出空 + exit 0），绝不卡住用户会话。
- `loop-config.json` 的 `enabled` 默认 `false`；`maxAutoTurns` 默认 `10`（硬上限，防跑飞）。
- 默认 `continuePrompt` = `"继续下一步。如果任务已全部完成，请在回复中输出 [[ALL_DONE]]。"`；默认 `doneMarker` = `"[[ALL_DONE]]"`。
- 命名管道名：`\\.\pipe\autoclaude-status`。IPC 帧为一行 UTF-8 JSON 以 `\n` 结尾。
- Node 脚本用 CommonJS（`require`），与现有 `statusline.js` 一致。
- C++ 遵循现有风格：`WM_APP_*` 消息 + PostMessage 把堆指针交给 UI 线程 delete；worker 线程用 `std::thread` + `std::atomic<bool> stop_`。
- Node 测试文件命名 `*.test.js`，用 `node --test` 运行；测试须能在临时目录跑，不污染真实 `$AC_DIR`（通过环境变量 `AUTOCLAUDE_DIR` 覆盖根目录）。

---

## 阶段 1：核心闭环（Stop hook，不依赖 GUI）

### Task 1: hook 的路径与配置加载模块

**Files:**
- Create: `hooks/lib/paths.js`
- Create: `hooks/lib/config.js`
- Test: `hooks/lib/config.test.js`

**Interfaces:**
- Produces `paths.js`:
  - `acDir()` → string：返回 `$AC_DIR`，优先读环境变量 `AUTOCLAUDE_DIR`，否则 `path.join(os.homedir(), '.claude', 'autoclaude')`。
  - `configPath()` → string：`path.join(acDir(), 'loop-config.json')`
  - `statePath()` → string：`path.join(acDir(), 'loop-state.json')`
  - `globalStopFlag()` → string：`path.join(acDir(), 'stop-flag')`
  - `sessionStopFlag(sessionId)` → string：`path.join(acDir(), 'stop-' + sessionId + '.flag')`
- Produces `config.js`:
  - `DEFAULT_CONFIG` → object：见下方代码。
  - `loadConfig()` → object：读 `configPath()`，与 `DEFAULT_CONFIG` 合并；文件缺失或解析失败返回 `DEFAULT_CONFIG` 的副本。

- [ ] **Step 1: Write the failing test**

```js
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node --test hooks/lib/config.test.js`
Expected: FAIL，`Cannot find module './config.js'`。

- [ ] **Step 3: Write minimal implementation**

```js
// hooks/lib/paths.js
const os = require('os');
const path = require('path');

function acDir() {
  return process.env.AUTOCLAUDE_DIR
    || path.join(os.homedir(), '.claude', 'autoclaude');
}
function configPath()  { return path.join(acDir(), 'loop-config.json'); }
function statePath()   { return path.join(acDir(), 'loop-state.json'); }
function globalStopFlag() { return path.join(acDir(), 'stop-flag'); }
function sessionStopFlag(sessionId) {
  return path.join(acDir(), 'stop-' + sessionId + '.flag');
}
module.exports = { acDir, configPath, statePath, globalStopFlag, sessionStopFlag };
```

```js
// hooks/lib/config.js
const fs = require('fs');
const { configPath } = require('./paths.js');

const DEFAULT_CONFIG = {
  enabled: false,
  maxAutoTurns: 10,
  continuePrompt: '继续下一步。如果任务已全部完成，请在回复中输出 [[ALL_DONE]]。',
  doneMarker: '[[ALL_DONE]]',
  onApiError: 'stop',
  scope: 'all',
  projectFilter: '',
};

function loadConfig() {
  try {
    const raw = fs.readFileSync(configPath(), 'utf8');
    const parsed = JSON.parse(raw);
    if (parsed && typeof parsed === 'object') {
      return { ...DEFAULT_CONFIG, ...parsed };
    }
  } catch { /* fall through */ }
  return { ...DEFAULT_CONFIG };
}
module.exports = { DEFAULT_CONFIG, loadConfig };
```

- [ ] **Step 4: Run test to verify it passes**

Run: `node --test hooks/lib/config.test.js`
Expected: PASS，3 个测试通过。

- [ ] **Step 5: Commit**

```bash
git add hooks/lib/paths.js hooks/lib/config.js hooks/lib/config.test.js
git commit -m "feat(hook): loop config + paths module with fail-safe defaults"
```

---

### Task 2: 轮次记账模块（loop-state 原子读写）

**Files:**
- Create: `hooks/lib/state.js`
- Test: `hooks/lib/state.test.js`

**Interfaces:**
- Consumes: `paths.js` 的 `statePath()`。
- Produces:
  - `loadState()` → object：读 `statePath()`，返回 `{ sessions: {} }` 结构；缺失/损坏返回 `{ sessions: {} }`。
  - `getTurns(state, sessionId)` → number：`state.sessions[sessionId]?.autoTurns ?? 0`。
  - `bumpTurns(state, sessionId)` → void：对该 session `autoTurns += 1`（不存在则建 `{autoTurns:1}`）。
  - `resetTurns(state, sessionId)` → void：删除该 session 记录。
  - `saveState(state)` → void：**原子写**（写临时文件 + `fs.renameSync`），目录不存在则 `mkdirSync(recursive)`。

- [ ] **Step 1: Write the failing test**

```js
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node --test hooks/lib/state.test.js`
Expected: FAIL，`Cannot find module './state.js'`。

- [ ] **Step 3: Write minimal implementation**

```js
// hooks/lib/state.js
const fs = require('fs');
const path = require('path');
const { statePath, acDir } = require('./paths.js');

function loadState() {
  try {
    const parsed = JSON.parse(fs.readFileSync(statePath(), 'utf8'));
    if (parsed && typeof parsed === 'object' && parsed.sessions
        && typeof parsed.sessions === 'object') {
      return parsed;
    }
  } catch { /* fall through */ }
  return { sessions: {} };
}

function getTurns(state, sessionId) {
  const s = state.sessions[sessionId];
  return (s && typeof s.autoTurns === 'number') ? s.autoTurns : 0;
}

function bumpTurns(state, sessionId) {
  const cur = getTurns(state, sessionId);
  state.sessions[sessionId] = { ...(state.sessions[sessionId] || {}), autoTurns: cur + 1 };
}

function resetTurns(state, sessionId) {
  delete state.sessions[sessionId];
}

function saveState(state) {
  fs.mkdirSync(acDir(), { recursive: true });
  const target = statePath();
  const tmp = target + '.' + process.pid + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify(state, null, 2));
  fs.renameSync(tmp, target);
}

module.exports = { loadState, getTurns, bumpTurns, resetTurns, saveState };
```

- [ ] **Step 4: Run test to verify it passes**

Run: `node --test hooks/lib/state.test.js`
Expected: PASS，3 个测试通过。

- [ ] **Step 5: Commit**

```bash
git add hooks/lib/state.js hooks/lib/state.test.js
git commit -m "feat(hook): loop-state turn accounting with atomic writes"
```

---

### Task 3: 决策核心 `decide()`（纯函数，Stop hook 的大脑）

**Files:**
- Create: `hooks/lib/decide.js`
- Test: `hooks/lib/decide.test.js`

**Interfaces:**
- Consumes: `config.js` 的形状、`state.js` 的 `getTurns`。
- Produces:
  - `decide(input, config, state)` → `{ action, reason?, resetSession?, bumpSession?, clearStopFlags? }`
    - `input`：`{ sessionId, cwd, lastAssistantMessage, stopFlagPresent }`
    - 返回 `action` 为 `'allow'`（放行，输出空）或 `'block'`（输出 block+reason）。
    - `reason`：block 时的提示词。
    - `bumpSession`：true 时调用方应 `bumpTurns` 并保存。
    - `resetSession`：true 时调用方应 `resetTurns` 并保存（doneMarker 命中 / 超上限）。
    - `clearStopFlags`：true 时调用方应删除停止标志文件。
  - 决策顺序（与 spec §5 一致）：
    1. `!config.enabled` → allow
    2. `scope==='project'` 且 `cwd` 不以 `projectFilter` 开头（大小写不敏感、非空时）→ allow
    3. `stopFlagPresent` → allow + `clearStopFlags:true`
    4. `lastAssistantMessage` 含 `doneMarker` → allow + `resetSession:true`
    5. `getTurns >= maxAutoTurns` → allow + `resetSession:true`
    6. 否则 → block + `reason=continuePrompt` + `bumpSession:true`

- [ ] **Step 1: Write the failing test**

```js
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node --test hooks/lib/decide.test.js`
Expected: FAIL，`Cannot find module './decide.js'`。

- [ ] **Step 3: Write minimal implementation**

```js
// hooks/lib/decide.js
const { getTurns } = require('./state.js');

function decide(input, config, state) {
  if (!config.enabled) return { action: 'allow' };

  if (config.scope === 'project' && config.projectFilter) {
    const cwd = String(input.cwd || '').toLowerCase();
    const pf = String(config.projectFilter).toLowerCase();
    if (!cwd.startsWith(pf)) return { action: 'allow' };
  }

  if (input.stopFlagPresent) {
    return { action: 'allow', clearStopFlags: true };
  }

  const msg = String(input.lastAssistantMessage || '');
  if (config.doneMarker && msg.includes(config.doneMarker)) {
    return { action: 'allow', resetSession: true };
  }

  if (getTurns(state, input.sessionId) >= config.maxAutoTurns) {
    return { action: 'allow', resetSession: true };
  }

  return { action: 'block', reason: config.continuePrompt, bumpSession: true };
}
module.exports = { decide };
```

- [ ] **Step 4: Run test to verify it passes**

Run: `node --test hooks/lib/decide.test.js`
Expected: PASS，7 个测试通过。

- [ ] **Step 5: Commit**

```bash
git add hooks/lib/decide.js hooks/lib/decide.test.js
git commit -m "feat(hook): pure decide() core for auto-continue"
```

---

### Task 4: Stop hook 入口脚本 `autoclaude-loop.js`（组装 + IO + 输出）

**Files:**
- Create: `hooks/autoclaude-loop.js`
- Test: `hooks/autoclaude-loop.test.js`

**Interfaces:**
- Consumes: `lib/config.js`、`lib/state.js`、`lib/decide.js`、`lib/paths.js`。
- Produces: 一个可执行 hook——从 stdin 读 Claude Code 的 Stop hook JSON，决策后：
  - allow → 输出空字符串，`process.exitCode = 0`。
  - block → `process.stdout.write(JSON.stringify({ decision: 'block', reason }))`，exit 0。
  - 执行 `bumpSession`/`resetSession`（saveState）、`clearStopFlags`（删文件）副作用。
  - **fail-open**：任何异常 → 输出空 + exit 0。
- 为可测，导出 `runHook(inputJson)` → `{ stdout, sideEffects }`（不直接碰 process），入口 `main()` 才读真实 stdin/写 stdout。
  - `runHook(inputJson)` 返回 `{ stdout: string }`，并执行文件副作用（saveState / 删 flag）。

- [ ] **Step 1: Write the failing test**

```js
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
  for (const m of ['./hooks/lib/paths.js','./hooks/lib/config.js',
                   './hooks/lib/state.js','./hooks/lib/decide.js',
                   './hooks/autoclaude-loop.js']) {
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
    const { runHook } = require('./hooks/autoclaude-loop.js');
    const r = runHook({ session_id: 's1', cwd: 'C:\\p', last_assistant_message: 'x' });
    assert.strictEqual(r.stdout, '');
  });
});

test('enabled below max -> block JSON and state bumped', () => {
  withTmp((dir) => {
    writeCfg(dir, { enabled: true, maxAutoTurns: 5, continuePrompt: 'GO' });
    const { runHook } = require('./hooks/autoclaude-loop.js');
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
    const { runHook } = require('./hooks/autoclaude-loop.js');
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
    const { runHook } = require('./hooks/autoclaude-loop.js');
    const r = runHook({ session_id: 's1', cwd: 'C:\\p', last_assistant_message: 'x' });
    assert.strictEqual(r.stdout, '');
    assert.strictEqual(fs.existsSync(flag), false);
  });
});

test('malformed config -> fail-open empty stdout', () => {
  withTmp((dir) => {
    fs.writeFileSync(path.join(dir, 'loop-config.json'), '{ bad');
    const { runHook } = require('./hooks/autoclaude-loop.js');
    const r = runHook({ session_id: 's1', cwd: 'C:\\p', last_assistant_message: 'x' });
    assert.strictEqual(r.stdout, '');
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node --test hooks/autoclaude-loop.test.js`
Expected: FAIL，`Cannot find module './hooks/autoclaude-loop.js'`。

- [ ] **Step 3: Write minimal implementation**

```js
#!/usr/bin/env node
// hooks/autoclaude-loop.js — Claude Code Stop hook: auto-continue loop.
const fs = require('fs');
const { loadConfig } = require('./lib/config.js');
const { loadState, bumpTurns, resetTurns, saveState } = require('./lib/state.js');
const { decide } = require('./lib/decide.js');
const { globalStopFlag, sessionStopFlag } = require('./lib/paths.js');

function runHook(inputJson) {
  try {
    const config = loadConfig();
    const state = loadState();
    const sessionId = String(inputJson.session_id || '');
    const input = {
      sessionId,
      cwd: inputJson.cwd || (inputJson.workspace && inputJson.workspace.current_dir) || '',
      lastAssistantMessage: inputJson.last_assistant_message || '',
      stopFlagPresent:
        (sessionId && fs.existsSync(sessionStopFlag(sessionId))) ||
        fs.existsSync(globalStopFlag()),
    };

    const d = decide(input, config, state);

    if (d.clearStopFlags) {
      for (const f of [globalStopFlag(), sessionId && sessionStopFlag(sessionId)]) {
        try { if (f && fs.existsSync(f)) fs.unlinkSync(f); } catch {}
      }
    }
    if (d.bumpSession) { bumpTurns(state, sessionId); saveState(state); }
    if (d.resetSession) { resetTurns(state, sessionId); saveState(state); }

    if (d.action === 'block') {
      return { stdout: JSON.stringify({ decision: 'block', reason: d.reason }) };
    }
    return { stdout: '' };
  } catch {
    return { stdout: '' };  // fail-open
  }
}

function main() {
  const chunks = [];
  process.stdin.on('data', (c) => chunks.push(c));
  process.stdin.on('end', () => {
    let input = {};
    try { input = JSON.parse(Buffer.concat(chunks).toString() || '{}'); } catch {}
    const { stdout } = runHook(input);
    if (stdout) process.stdout.write(stdout);
    process.exitCode = 0;
  });
}

if (require.main === module) main();
module.exports = { runHook };
```

- [ ] **Step 4: Run test to verify it passes**

Run: `node --test hooks/autoclaude-loop.test.js`
Expected: PASS，5 个测试通过。

- [ ] **Step 5: Run全部 hook 测试确认无回归**

Run: `node --test hooks/`
Expected: PASS，全部（Task 1-4）测试通过。

- [ ] **Step 6: Commit**

```bash
git add hooks/autoclaude-loop.js hooks/autoclaude-loop.test.js
git commit -m "feat(hook): autoclaude-loop Stop hook entrypoint (fail-open)"
```

---

### Task 5: 安装脚本与文档（配置 hook 到 settings.json）

**Files:**
- Create: `hooks/install-hook.js`
- Create: `hooks/README.md`
- Modify: `README.md`（追加"Auto-continue loop"一节）

**Interfaces:**
- Consumes: 无（独立 CLI）。
- Produces: `node hooks/install-hook.js` 幂等地：
  - 把 `hooks/` 复制到 `~/.claude/autoclaude-hooks/`（或原地引用，见实现）。
  - 在 `~/.claude/settings.json` 的 `hooks.Stop` 追加一条 `{ "matcher": "", "hooks": [{ "type": "command", "command": "node \"<abs>/autoclaude-loop.js\"" }] }`，已存在同 command 则跳过。
  - 创建 `~/.claude/autoclaude/loop-config.json`（若不存在）写入 DEFAULT_CONFIG。
- 无独立单元测试（纯安装副作用）；验证靠 Step 手动跑 + 检查 settings.json。

- [ ] **Step 1: 写 install-hook.js**

```js
#!/usr/bin/env node
// hooks/install-hook.js — register autoclaude-loop as a Stop hook (idempotent).
const fs = require('fs');
const os = require('os');
const path = require('path');
const { DEFAULT_CONFIG } = require('./lib/config.js');
const { acDir, configPath } = require('./lib/paths.js');

const claudeDir = path.join(os.homedir(), '.claude');
const settingsPath = path.join(claudeDir, 'settings.json');
const hookAbs = path.join(__dirname, 'autoclaude-loop.js').split(path.sep).join('/');
const command = `node "${hookAbs}"`;

fs.mkdirSync(acDir(), { recursive: true });
if (!fs.existsSync(configPath())) {
  fs.writeFileSync(configPath(), JSON.stringify(DEFAULT_CONFIG, null, 2));
  console.log('created', configPath());
}

let settings = {};
if (fs.existsSync(settingsPath)) {
  try { settings = JSON.parse(fs.readFileSync(settingsPath, 'utf8')); } catch { settings = {}; }
}
settings.hooks = settings.hooks || {};
settings.hooks.Stop = settings.hooks.Stop || [];
const already = JSON.stringify(settings.hooks.Stop).includes(hookAbs);
if (already) {
  console.log('Stop hook already registered.');
} else {
  settings.hooks.Stop.push({ matcher: '', hooks: [{ type: 'command', command }] });
  fs.writeFileSync(settingsPath, JSON.stringify(settings, null, 2));
  console.log('registered Stop hook in', settingsPath);
}
```

- [ ] **Step 2: 干跑验证（用临时 HOME 不污染真实环境）**

Run（bash）:
```bash
AC_TMP=$(mktemp -d)
HOME="$AC_TMP" USERPROFILE="$AC_TMP" AUTOCLAUDE_DIR="$AC_TMP/.claude/autoclaude" node hooks/install-hook.js
cat "$AC_TMP/.claude/settings.json"
```
Expected: 输出包含 `hooks.Stop` 且 command 指向 `autoclaude-loop.js`；`loop-config.json` 已创建。

- [ ] **Step 3: 再跑一次确认幂等**

Run: 同上命令再执行一次。
Expected: 打印 `Stop hook already registered.`，settings.json 不重复追加。

- [ ] **Step 4: 写 hooks/README.md 与主 README 一节**

`hooks/README.md` 内容要点（安装、配置字段说明、如何开启 `enabled`、如何喊停、如何跑测试 `node --test hooks/`）。主 `README.md` 追加"Auto-continue loop"小节，链接到 `hooks/README.md` 和 spec。

- [ ] **Step 5: Commit**

```bash
git add hooks/install-hook.js hooks/README.md README.md
git commit -m "feat(hook): idempotent installer + docs for auto-continue loop"
```

---

## 阶段 2：观测（sensor 广播 + AutoClaude IPC 服务端 + UI 显示）

### Task 6: statusline.js 扩展为传感器（广播状态帧）

**Files:**
- Create: `hooks/lib/broadcast.js`
- Test: `hooks/lib/broadcast.test.js`
- Modify: `D:\Razer\Anne\tool\CCStatusLine\statusline.js`（在 `process.stdin.on('end')` 末尾追加广播调用，包 try/catch）

**Interfaces:**
- Produces `broadcast.js`:
  - `buildFrame({ sessionId, cwd, transcriptPath })` → string：读 `loop-config.json` 的 `enabled` 和 `loop-state.json` 的该 session `autoTurns`，返回一行 JSON + `\n`。字段：`sessionId, cwd, transcriptPath, loopEnabled, autoTurns`。
  - `sendFrame(frame)` → void：连接命名管道 `\\.\pipe\autoclaude-status` 写入 frame；管道不存在（服务端未开）→ 静默返回。**绝不抛异常、绝不阻塞**（用短超时 / 立即 `end()`）。
- `buildFrame` 有单元测试；`sendFrame` 只保证不抛（无服务端时静默）。

- [ ] **Step 1: Write the failing test**

```js
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node --test hooks/lib/broadcast.test.js`
Expected: FAIL，`Cannot find module './broadcast.js'`。

- [ ] **Step 3: Write minimal implementation**

```js
// hooks/lib/broadcast.js
const net = require('net');
const { loadConfig } = require('./config.js');
const { loadState, getTurns } = require('./state.js');

const PIPE = '\\\\.\\pipe\\autoclaude-status';

function buildFrame({ sessionId, cwd, transcriptPath }) {
  let loopEnabled = false, autoTurns = 0;
  try { loopEnabled = !!loadConfig().enabled; } catch {}
  try { autoTurns = getTurns(loadState(), sessionId); } catch {}
  return JSON.stringify({
    sessionId: sessionId || '', cwd: cwd || '',
    transcriptPath: transcriptPath || '', loopEnabled, autoTurns,
  }) + '\n';
}

function sendFrame(frame) {
  try {
    const sock = net.connect(PIPE);
    sock.on('error', () => { try { sock.destroy(); } catch {} });
    sock.on('connect', () => { try { sock.end(frame); } catch {} });
  } catch { /* never block the status line */ }
}
module.exports = { buildFrame, sendFrame, PIPE };
```

- [ ] **Step 4: Run test to verify it passes**

Run: `node --test hooks/lib/broadcast.test.js`
Expected: PASS，2 个测试通过。

- [ ] **Step 5: 接入 statusline.js（不破坏其现有输出）**

在 `D:\Razer\Anne\tool\CCStatusLine\statusline.js` 的 `process.stdin.on('end', ...)` 回调**最后**、`process.stdout.write(...)` 之后追加（用绝对路径 require 本仓库的 broadcast，或把 broadcast 复制进该目录——实现时二选一并在 hooks/README 注明）：

```js
  // --- AutoClaude sensor broadcast (best-effort, never blocks) ---
  try {
    const { buildFrame, sendFrame } = require('<ABS>/hooks/lib/broadcast.js');
    const sid = (data.session_id) || (tp && path.basename(tp, '.jsonl')) || '';
    sendFrame(buildFrame({ sessionId: sid, cwd: currentDir, transcriptPath: tp }));
  } catch { /* ignore */ }
```

- [ ] **Step 6: 手动验证 statusline 仍正常**

Run（bash）:
```bash
echo '{"model":{"display_name":"test"},"workspace":{"current_dir":"C:\\p"}}' | node "D:/Razer/Anne/tool/CCStatusLine/statusline.js"
```
Expected: 正常输出状态栏文字（首行含 `[test]`），无异常、无阻塞。

- [ ] **Step 7: Commit**

```bash
git add hooks/lib/broadcast.js hooks/lib/broadcast.test.js
git commit -m "feat(sensor): status frame builder + non-blocking pipe sender"
```

---

### Task 7: AutoClaude IPC 服务端（命名管道，C++）

**Files:**
- Create: `src/ipc/status_pipe.h`
- Create: `src/ipc/status_pipe.cpp`
- Modify: `src/app.h`（新增 `WM_APP_LOOPSTATUS` + `LoopStatus` 结构）
- Modify: `CMakeLists.txt`（加入 `src/ipc/status_pipe.cpp`）

**Interfaces:**
- Consumes: `HWND`（PostMessage 目标）。
- Produces:
  - `app.h`:
    ```cpp
    #define WM_APP_LOOPSTATUS (WM_APP + 4)  // LPARAM = LoopStatus*（UI 线程 delete）
    struct LoopStatus {
        std::wstring sessionId;
        std::wstring cwd;
        bool loopEnabled = false;
        int  autoTurns = 0;
    };
    ```
  - `status_pipe.h`:
    ```cpp
    class StatusPipeServer {
    public:
        void Start(HWND hwnd);
        void Stop();
        ~StatusPipeServer();
    private:
        void Run();
        HWND hwnd_ = nullptr;
        std::thread th_;
        std::atomic<bool> stop_{false};
        HANDLE pipe_ = INVALID_HANDLE_VALUE;
    };
    ```
- 实现：worker 线程循环 `CreateNamedPipeW(L"\\\\.\\pipe\\autoclaude-status", PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 0, 4096, 0, nullptr)` → `ConnectNamedPipe` → `ReadFile` 累积到 `\n` → 用 nlohmann::json 解析一帧 → `new LoopStatus` + `PostMessageW(hwnd_, WM_APP_LOOPSTATUS, 0, (LPARAM)ptr)` → `DisconnectNamedPipe` 回到循环。`Stop()` 置 `stop_`，用 `CreateFileW` 自连一次唤醒阻塞的 `ConnectNamedPipe`，`CloseHandle`，`join`。

**注意（无自动化单测；C++ IPC 靠集成验证）**：本 task 交付即"能编译 + 手动能收帧"。

- [ ] **Step 1: 改 app.h 加消息与结构**

按上方 `app.h` 接口，在现有 `WM_APP_SESSIONS` 定义后追加 `WM_APP_LOOPSTATUS` 与 `LoopStatus`。

- [ ] **Step 2: 写 status_pipe.h / status_pipe.cpp**

按上方接口实现。`Run()` 关键骨架：

```cpp
void StatusPipeServer::Run() {
    const wchar_t* name = L"\\\\.\\pipe\\autoclaude-status";
    while (!stop_.load()) {
        HANDLE p = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 0, 4096, 0, nullptr);
        if (p == INVALID_HANDLE_VALUE) { Sleep(200); continue; }
        pipe_ = p;
        BOOL ok = ConnectNamedPipe(p, nullptr) ? TRUE
                  : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (stop_.load()) { CloseHandle(p); break; }
        if (ok) {
            std::string acc; char buf[1024]; DWORD n = 0;
            while (ReadFile(p, buf, sizeof(buf), &n, nullptr) && n > 0) {
                acc.append(buf, n);
                size_t nl;
                while ((nl = acc.find('\n')) != std::string::npos) {
                    std::string line = acc.substr(0, nl);
                    acc.erase(0, nl + 1);
                    EmitFrame(line);   // 解析 + PostMessage
                }
            }
        }
        DisconnectNamedPipe(p);
        CloseHandle(p);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}
```

`EmitFrame(const std::string&)`：nlohmann::json 解析，取 `sessionId/cwd/loopEnabled/autoTurns`，UTF-8→wide（复用 config.cpp 的转换思路，或本地实现一个 `ToWide`），`PostMessageW(hwnd_, WM_APP_LOOPSTATUS, 0, (LPARAM)new LoopStatus{...})`。解析失败则忽略该行。

`Stop()`:
```cpp
void StatusPipeServer::Stop() {
    stop_.store(true);
    // 自连一次唤醒阻塞的 ConnectNamedPipe
    HANDLE c = CreateFileW(L"\\\\.\\pipe\\autoclaude-status",
        GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (c != INVALID_HANDLE_VALUE) CloseHandle(c);
    if (th_.joinable()) th_.join();
}
```

- [ ] **Step 3: CMakeLists.txt 加源文件**

在 `add_executable` 列表加入 `src/ipc/status_pipe.cpp`。

- [ ] **Step 4: 构建**

Run: `cmd //c build.bat`
Expected: `[build] OK: build\Release\AutoClaude.exe`，无编译错误。

- [ ] **Step 5: Commit**

```bash
git add src/ipc/status_pipe.h src/ipc/status_pipe.cpp src/app.h CMakeLists.txt
git commit -m "feat(ipc): named-pipe status server + WM_APP_LOOPSTATUS"
```

---

### Task 8: AutoClaude 启动 IPC 服务端并在 UI 显示 loop 徽标

**Files:**
- Modify: `src/ui/window.cpp`（AppCtx 加 `StatusPipeServer`；启动/停止；`SessionRow` 加 `autoTurns/loopEnabled`；`WM_APP_LOOPSTATUS` 处理；PaintRow 底行加 "loop N/M"）
- Modify: `src/ui/window.cpp` 顶部 `#include "../ipc/status_pipe.h"`

**Interfaces:**
- Consumes: Task 7 的 `StatusPipeServer`、`WM_APP_LOOPSTATUS`、`LoopStatus`。
- Produces: UI 副作用；无新对外接口。

- [ ] **Step 1: SessionRow 与 AppCtx 扩展**

`SessionRow` 增加：
```cpp
int  autoTurns = 0;
bool loopEnabled = false;
```
`AppCtx` 增加成员：`StatusPipeServer pipe;` 并 `#include "../ipc/status_pipe.h"`。
`RunApp` 里 `ctx->watcher.Start(...)` 之后加 `ctx->pipe.Start(hwnd);`，消息循环结束后 `ctx->watcher.Stop();` 旁加 `ctx->pipe.Stop();`。

- [ ] **Step 2: 处理 WM_APP_LOOPSTATUS**

新增 handler：
```cpp
void OnLoopStatus(HWND hwnd, LoopStatus* ls) {
    AppCtx* c = Ctx(hwnd);
    if (!c) { delete ls; return; }
    // 按 sessionId 后缀匹配到 shortId，或按 path 包含匹配
    for (auto& r : c->sessions) {
        if (!ls->sessionId.empty() &&
            r.path.find(ls->sessionId) != std::wstring::npos) {
            r.autoTurns = ls->autoTurns;
            r.loopEnabled = ls->loopEnabled;
            break;
        }
    }
    delete ls;
    InvalidateRect(hwnd, nullptr, FALSE);
}
```
在 WndProc 加 `case WM_APP_LOOPSTATUS: OnLoopStatus(hwnd, (LoopStatus*)lp); return 0;`

- [ ] **Step 3: PaintRow 底行追加 loop 徽标**

在 PaintRow 组装 `bot` 字符串处（events 之后）追加：
```cpp
if (r.loopEnabled) {
    bot += L"  ·  loop ";
    bot += std::to_wstring(r.autoTurns);
    bot += L"/";
    bot += std::to_wstring(c->cfg.maxAutoTurns);  // 见 Step 4：cfg 需含 maxAutoTurns
}
```
（`PaintRow` 需要能访问 maxAutoTurns——把它作为参数传入，或从全局 cfg 传入。实现时给 `PaintRow` 增加 `int maxAutoTurns` 参数，调用处传 `c->cfg.maxAutoTurns`。）

- [ ] **Step 4: Config 增加 maxAutoTurns（供 UI 显示分母）**

在 `src/config.h` 的 `Config` 增加 `int maxAutoTurns = 10;`，`config.cpp` 的 Load/Save 各加一行读写（键名 `maxAutoTurns`）。**注意**：真正的 loop 配置源是 `loop-config.json`；此处 AutoClaude 仅为显示分母缓存一份，MVP 可接受轻微冗余（在 README 注明以 loop-config.json 为准）。

- [ ] **Step 5: 构建**

Run: `cmd //c build.bat`
Expected: `[build] OK`，无编译错误。

- [ ] **Step 6: 端到端手动验证**

1. 启动 `build/Release/AutoClaude.exe`。
2. bash 手动写一帧到管道：
```bash
printf '{"sessionId":"deadbeef","cwd":"C:\\\\p","loopEnabled":true,"autoTurns":3}\n' \
  > //./pipe/autoclaude-status || echo "（若 bash 无法写命名管道，用下方 node 一行）"
node -e "const net=require('net');const s=net.connect('\\\\\\\\.\\\\pipe\\\\autoclaude-status');s.on('connect',()=>s.end('{\"sessionId\":\"deadbeef\",\"cwd\":\"C:\\\\p\",\"loopEnabled\":true,\"autoTurns\":3}\n'));s.on('error',console.error);"
```
Expected: 若有 shortId 含 `deadbeef` 的会话行，其底行出现 `loop 3/10`。（无匹配会话时不显示属正常——广播先于会话发现。）

- [ ] **Step 7: Commit**

```bash
git add src/ui/window.cpp src/config.h src/config.cpp
git commit -m "feat(ui): start pipe server + show loop N/M badge on session rows"
```

---

## 阶段 3：干预 UI（Stop Loop 按钮 + enabled 开关）

### Task 9: Stop Loop 按钮（写全局停止标志）

**Files:**
- Modify: `src/ui/window.cpp`（Layout 加一个按钮 region；Paint 绘制；OnLButtonDown 点击写 `stop-flag`；OnMouseMove hover）
- Create: `src/ipc/ac_paths.h`（C++ 侧解析 `$AC_DIR` 与 stop-flag 路径，供 window.cpp 用）

**Interfaces:**
- Produces `ac_paths.h`（header-only 内联函数）：
  - `std::wstring AcDir()`：读环境变量 `AUTOCLAUDE_DIR`（`GetEnvironmentVariableW`），否则 `%USERPROFILE%\.claude\autoclaude`。
  - `std::wstring StopFlagPath()`：`AcDir() + L"\\stop-flag"`。
  - `bool WriteStopFlag()`：`CreateDirectoryW`(递归确保 AcDir) + 写空文件到 StopFlagPath；成功 true。

- [ ] **Step 1: 写 ac_paths.h**

```cpp
#pragma once
#include <windows.h>
#include <string>
#include <fstream>

inline std::wstring AcDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"AUTOCLAUDE_DIR", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    wchar_t up[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
    return std::wstring(up) + L"\\.claude\\autoclaude";
}
inline std::wstring StopFlagPath() { return AcDir() + L"\\stop-flag"; }
inline bool WriteStopFlag() {
    std::wstring dir = AcDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr); // shlwapi/shell32
    std::wstring p = StopFlagPath();
    std::ofstream f(p.c_str(), std::ios::binary | std::ios::trunc);
    return (bool)f;
}
```
（若不想引 `SHCreateDirectoryExW`，用逐级 `CreateDirectoryW`；实现时择一，确保 CMake 链接的库满足。现有已链接 `shell32`。）

- [ ] **Step 2: Layout 增加 stopLoopBtn region 并重排底部按钮**

当前底部是 Cancel + Pause 两个半宽按钮。改为三等分或在 Pause 上方加一行。MVP 最简：把底部两按钮行改为三按钮（Cancel / Stop Loop / Pause）。在 `Layout::Recompute` 里把 `cancelBtn`/`startBtn` 的半宽布局改为三等分，新增 `Region stopLoopBtn;`。

- [ ] **Step 3: Paint 绘制 Stop Loop 按钮**

参照现有 Cancel 按钮绘制风格，两行标签 "Stop Loop" / "halt auto-continue"。启用色：当任一会话 `loopEnabled` 为真时用 warn 色，否则 muted。

- [ ] **Step 4: OnLButtonDown 处理点击**

```cpp
if (L.stopLoopBtn.Contains(x, y)) {
    WriteStopFlag();
    c->doneMsg = L"stop-flag written · loop will halt next turn";
    InvalidateRect(hwnd, nullptr, FALSE);
    return;
}
```
并在 OnMouseMove/OnMouseLeave/WM_SETCURSOR 里把 `stopLoopBtn` 纳入 hover 处理（新增 `bool hoverStopLoop;`）。

- [ ] **Step 5: 构建**

Run: `cmd //c build.bat`
Expected: `[build] OK`。

- [ ] **Step 6: 手动验证**

启动 exe → 点 "Stop Loop" → 检查 `%USERPROFILE%\.claude\autoclaude\stop-flag` 文件出现；状态行显示提示。删除该文件后再点可重现。

- [ ] **Step 7: Commit**

```bash
git add src/ipc/ac_paths.h src/ui/window.cpp
git commit -m "feat(ui): Stop Loop button writes stop-flag to halt auto-continue"
```

---

### Task 10: enabled 开关切换（编辑 loop-config.json）

**Files:**
- Modify: `src/ipc/ac_paths.h`（加 `LoopConfigPath()` + `ToggleLoopEnabled()`）
- Modify: `src/ui/window.cpp`（Help 卡片补充 loop 说明；在标题栏或状态行加一个 loop toggle 命中区）

**Interfaces:**
- Produces `ac_paths.h`：
  - `std::wstring LoopConfigPath()`：`AcDir() + L"\\loop-config.json"`。
  - `bool ReadLoopEnabled()`：读 json 的 `enabled`（用 nlohmann::json；window.cpp 已可用），缺失/失败返回 false。
  - `bool ToggleLoopEnabled()`：读→翻转 `enabled`→写回（保留其他字段），返回新值。

- [ ] **Step 1: 扩展 ac_paths.h**

用 nlohmann::json 实现 `ReadLoopEnabled` / `ToggleLoopEnabled`（读整个对象，翻转 `enabled`，`dump(2)` 写回；文件不存在则以 `{"enabled":true}` 起步——但更稳妥是要求先跑过 install-hook.js 生成完整默认配置，见 README）。

```cpp
#include <json.hpp>
inline std::wstring LoopConfigPath() { return AcDir() + L"\\loop-config.json"; }
inline bool ReadLoopEnabled() {
    try {
        std::ifstream f(LoopConfigPath(), std::ios::binary);
        if (!f) return false;
        nlohmann::json j; f >> j;
        return j.value("enabled", false);
    } catch (...) { return false; }
}
inline bool ToggleLoopEnabled() {
    nlohmann::json j;
    try { std::ifstream f(LoopConfigPath(), std::ios::binary); if (f) f >> j; } catch (...) { j = nlohmann::json::object(); }
    if (!j.is_object()) j = nlohmann::json::object();
    bool nv = !j.value("enabled", false);
    j["enabled"] = nv;
    std::wstring dir = AcDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::ofstream o(LoopConfigPath(), std::ios::binary | std::ios::trunc);
    o << j.dump(2);
    return nv;
}
```

- [ ] **Step 2: 状态行加 loop toggle 命中区**

在状态行右侧空白处画一个小 pill "LOOP: ON/OFF"（颜色随 `ReadLoopEnabled()`）。新增 `Region loopToggle;` 到 Layout，Paint 里绘制，OnLButtonDown 点击调用 `ToggleLoopEnabled()` 并 `InvalidateRect`。启动时读一次真实值缓存到 `AppCtx::loopEnabledCached`，每次 toggle 后更新。

- [ ] **Step 3: Help 卡片补充 loop 说明**

在 `PaintHelpOverlay` 加一个 Section："Auto-continue loop"：解释 LOOP toggle、Stop Loop 按钮、`[[ALL_DONE]]` 标记、maxAutoTurns 上限。

- [ ] **Step 4: 构建**

Run: `cmd //c build.bat`
Expected: `[build] OK`。

- [ ] **Step 5: 手动验证**

启动 exe → 点 LOOP toggle → 检查 `loop-config.json` 的 `enabled` 翻转 → 再点翻回。Help 卡片 F1 打开能看到新 Section。

- [ ] **Step 6: Commit**

```bash
git add src/ipc/ac_paths.h src/ui/window.cpp
git commit -m "feat(ui): LOOP on/off toggle + help section for auto-continue"
```

---

## Self-Review 结果

**Spec 覆盖**：
- §2 三角色 → Task 4（hook 执行端）、Task 6（sensor）、Task 7-8（IPC+UI 大脑）✓
- §3.1 loop-config.json → Task 1 ✓；§3.2 loop-state.json → Task 2 ✓；§3.3 stop-flag → Task 4 读 + Task 9 写 ✓；§3.4 IPC 帧 → Task 6 buildFrame + Task 7 服务端 ✓
- §4.2 IPC 服务端 → Task 7 ✓；§4.3 app.h → Task 7 ✓；§4.4 UI → Task 8 ✓
- §5 hook 逻辑六步 → Task 3 decide() 全覆盖 ✓
- §6 安全（fail-open/maxAutoTurns/doneMarker/stop-flag/enabled 默认 false）→ 全局约束 + 各 task ✓
- §6 开放问题 1（计数重置）：MVP 采用 doneMarker + maxAutoTurns 重置；"人工输入重置"未做，属已知限制，记入 hooks/README（不阻塞）。
- §7 验证 → hook 单元（Task 1-4,6）、IPC 端到端（Task 8 Step 6）、真实集成（下方"最终集成验证"）✓
- §8 分期 → 三阶段对应 Task 1-5 / 6-8 / 9-10 ✓

**Placeholder 扫描**：无 TBD/TODO；每个代码步骤含完整代码。Task 7/8/9 的 C++ IPC/GUI 无自动化单测，已明确标注靠集成/手动验证并给出具体命令。

**类型一致性**：`decide()` 返回字段（action/reason/bumpSession/resetSession/clearStopFlags）在 Task 3 定义、Task 4 消费一致；`LoopStatus`（sessionId/cwd/loopEnabled/autoTurns）Task 7 定义、Task 8 消费一致；`buildFrame` 帧字段与 `EmitFrame` 解析字段一致；`AcDir/StopFlagPath/LoopConfigPath` 命名跨 Task 9/10 一致。

## 最终集成验证（全部 task 完成后）

1. `node hooks/install-hook.js` 注册 hook 并生成默认 `loop-config.json`。
2. 编辑 `loop-config.json` 设 `"enabled": true, "maxAutoTurns": 3`。
3. 在一个真实项目里启动 `claude`，给一个多步小任务（如"分 3 步重构这个函数，每步做完停下"）。
4. 观察 Claude 每轮结束后被 hook 自动续跑；到第 3 轮触发 maxAutoTurns 或 Claude 输出 `[[ALL_DONE]]` 后停止。
5. 启动 AutoClaude.exe，观察对应会话行显示 `loop N/3`。
6. 中途点 AutoClaude 的 "Stop Loop"，确认下一轮 Claude 停止（stop-flag 生效后被 hook 删除）。
7. `node --test hooks/` 全绿。
