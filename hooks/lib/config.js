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
