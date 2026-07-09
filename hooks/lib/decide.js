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
