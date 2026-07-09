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
  fs.mkdirSync(claudeDir, { recursive: true });
  fs.writeFileSync(settingsPath, JSON.stringify(settings, null, 2));
  console.log('registered Stop hook in', settingsPath);
}
