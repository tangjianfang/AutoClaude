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
