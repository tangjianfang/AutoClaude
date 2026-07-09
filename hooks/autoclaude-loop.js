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
