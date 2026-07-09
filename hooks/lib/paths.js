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
