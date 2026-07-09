# AutoClaude 自动接力循环（Auto-Continue Loop）

让 Claude Code 在完成一轮任务后，自动注入"继续下一步"提示词循环推进，直到满足停止条件。

核心是一个 Claude Code **Stop hook**（`autoclaude-loop.js`）：Claude 每轮想结束时触发，
若未满足停止条件就返回 `{"decision":"block","reason":"<提示词>"}` 让 Claude 带着提示词继续。
这是官方支持的机制，**不需要 AutoClaude GUI 运行即可自动接力**。

## 安装

```bash
node hooks/install-hook.js
```

幂等操作，会：
1. 创建 `%USERPROFILE%\.claude\autoclaude\loop-config.json`（若不存在，写入默认配置）。
2. 在 `%USERPROFILE%\.claude\settings.json` 的 `hooks.Stop` 追加本 hook（已存在则跳过）。

安装后**重启 Claude Code**（或开新会话）让 hook 生效。

## 开启

默认 `enabled: false`（安全）。编辑 `loop-config.json` 设 `"enabled": true` 即开启。

## 配置字段（`loop-config.json`）

| 字段 | 默认 | 说明 |
|------|------|------|
| `enabled` | `false` | 总开关。false 时 hook 立即放行，不干预。 |
| `maxAutoTurns` | `10` | 硬性上限。任一会话自动续跑到此数即强制停止（防跑飞）。 |
| `continuePrompt` | `"继续下一步。如果任务已全部完成，请在回复中输出 [[ALL_DONE]]。"` | 每轮注入的提示词。 |
| `doneMarker` | `"[[ALL_DONE]]"` | 完成标记。出现在最后一条 assistant 消息里即停止循环。 |
| `onApiError` | `"stop"` | api_error 后的策略（MVP 仅实现 stop）。 |
| `scope` | `"all"` | `"all"` 或 `"project"`。 |
| `projectFilter` | `""` | `scope=="project"` 时，只对 cwd 以此路径开头（大小写不敏感）的会话生效。 |

## 停止循环的三种方式

1. **Claude 自我终止**：Claude 在回复里输出 `doneMarker`（默认 `[[ALL_DONE]]`）。
2. **达到上限**：累计自动轮次达到 `maxAutoTurns`。
3. **手动喊停**：创建停止标志文件——AutoClaude GUI 的 "Stop Loop" 按钮会写它，或手动：
   - 全局：`%USERPROFILE%\.claude\autoclaude\stop-flag`
   - 单会话：`%USERPROFILE%\.claude\autoclaude\stop-<session_id>.flag`
   hook 读到后会删除该文件并放行当前轮。

## 状态文件

- `loop-state.json`：按 sessionId 记录 `autoTurns` 计数（hook 自动读写，原子写）。

## 安全性

- hook 全程 **fail-open**：任何异常、配置损坏、文件读不到 → 放行，绝不卡住会话。
- `enabled` 默认 false，`maxAutoTurns` 硬上限，随时可 stop-flag 喊停。

## 已知限制

- **计数不因人工输入重置**：hook 无法区分"自动续跑"与"用户手动输入新指令"触发的 turn，
  因此 `autoTurns` 只在命中 `doneMarker` 或达到 `maxAutoTurns` 时清零。若你在循环中途手动
  接管并希望重置计数，删除 `loop-state.json` 或对应 session 条目即可。

## 运行测试

```bash
node --test "hooks/**/*.test.js" "hooks/*.test.js"
```

（注意：Node v24 下 `node --test hooks/` 会把 `hooks` 当模块名解析而报错，需用上面的 glob 形式。）

## 传感器广播（可选，供 AutoClaude GUI 观测）

`lib/broadcast.js` 可被 statusline 脚本调用，每次状态栏刷新时经命名管道
`\\.\pipe\autoclaude-status` 把会话状态（`loopEnabled` / `autoTurns`）广播给正在运行的
AutoClaude GUI，用于显示 `loop N/M` 徽标。若 GUI 未运行，广播静默跳过，绝不阻塞状态栏。
