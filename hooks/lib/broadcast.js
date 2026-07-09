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
