# AutoClaude 自动接力循环（Auto-Continue Loop）设计文档

- 日期：2026-07-09
- 状态：已批准（方案 C）
- 作者：Mike Tang / Claude

## 1. 背景与目标

AutoClaude 当前是一个**只读**的 Win32 监控器：worker 线程 tail Claude Code 会话的
JSONL，解析出 `end_turn` / `api_error`，UI 线程驱动状态机走到"关机倒计时"。

本次扩展要把它从"监听 + 关机"升级为"监听 + **主动接力**"：当 Claude Code 完成一轮任务
（`end_turn`）后，自动把"继续下一步 / 进一步完善"之类的提示词送回会话，让 Claude 循环
推进，直到满足停止条件；断开/错误后按策略"继续"或停止。

### 关键事实（决定架构，已核实官方文档 2026-07-09）

1. **statusLine 命令是单向的**：Claude Code 通过 stdin 把 session JSON 喂给脚本，脚本的
   stdout 只被渲染成状态栏文字，**没有任何通道把文本喂回会话**。→ 只能当"传感器"。
2. **Stop / SubagentStop hook 是官方唯一干净的"自动继续"机制**：hook 在 Claude 想结束
   本轮时触发，返回 `{"decision":"block","reason":"<下一步提示词>"}`（或 `exit 2` +
   stderr）即可**阻止停止并让 Claude 带着 reason 继续**。无需模拟键盘。
3. **外部进程无法把提示词注入正在运行的交互式 TUI**（无官方 socket/pipe）。→ 回路逻辑
   必须住在 hook 里，而不是靠 AutoClaude "敲键盘"。

### 角色重新分工（方案 C）

| 角色 | 组件 | 职责 |
|------|------|------|
| 传感器 sensor | `statusline.js`（复用/扩展） | Claude 每次刷新状态栏时，把最新 session 状态经 IPC 广播给 AutoClaude |
| 执行端 actuator | `autoclaude-loop.js`（新增 Stop hook） | 任务想停时决策：满足停止条件→放行；否则返回 block + 下一步提示词 |
| 大脑 + UI controller | AutoClaude（现有 exe，扩展） | 配置循环规则、显示实时轮次/状态、提供"停止循环"喊停按钮 |

**方案 C 的核心特性**：hook 决策逻辑**自包含**（不开 AutoClaude 也能自动跑），但同时读一个
"停止标志文件"（AutoClaude 可写）并把每轮状态广播给 AutoClaude，做到**解耦 + 有韧性 +
可实时干预**。

## 2. 架构与数据流

```
Claude 跑完一轮 (end_turn)
        │
        ▼
  Stop hook 触发 (autoclaude-loop.js)
        │  1. 读 loop-config.json 的循环规则
        │  2. 读/更新 loop-state.json 的自动轮次计数
        │  3. 检查停止条件（下述任一满足即停）
        ├── 满足停止条件 → 输出空 / exit 0 放行 → Claude 正常停止
        └── 未满足 → 输出 {"decision":"block","reason":"<下一步提示词>"}
                     → Claude 带着提示词继续跑 ──┐
                                                 │
  statusline.js 每次刷新 ──IPC(命名管道)──► AutoClaude UI
        (广播：sessionId / 状态 / 轮次 / token)   │
                                                 │
  AutoClaude "停止循环"按钮 → 写 stop-flag 文件 ──┘（下一轮 hook 读到即放行）
```

所有 IPC 与状态文件放在一个固定目录：`%USERPROFILE%\.claude\autoclaude\`。

### 组件边界（每个单元的"做什么/怎么用/依赖谁"）

- **`autoclaude-loop.js`（Stop hook）** — 做什么：一次决策"是否让 Claude 继续"。
  怎么用：配在 `settings.json` 的 `hooks.Stop`。依赖：`loop-config.json`（规则）、
  `loop-state.json`（轮次）、`stop-flag`（喊停）、hook 输入 JSON 里的 `session_id` /
  `transcript_path` / `last_assistant_message`。无网络、无 GUI 依赖。
- **`statusline.js`（sensor，扩展现有）** — 做什么：显示状态栏 + 顺手广播状态。
  怎么用：Claude Code 已配的 statusLine 命令。依赖：命名管道客户端（写入即走，管道不存在
  就静默跳过，绝不阻塞状态栏渲染）。
- **AutoClaude IPC 服务端（新增 C++ 模块）** — 做什么：起一个命名管道服务端，接收 sensor
  广播，转成 UI 事件。怎么用：worker 线程内启动，PostMessage 给 UI 线程。依赖：现有
  `TranscriptWatcher` 同级，复用 `WM_APP_*` 消息机制。
- **AutoClaude 循环 UI（扩展 window.cpp）** — 做什么：显示每个会话的循环状态/轮次，提供
  "停止循环"按钮（写 stop-flag）。依赖：IPC 服务端事件 + 现有绘制层。

## 3. 数据契约

### 3.1 `loop-config.json`（AutoClaude 写，hook 读）
放在 `%USERPROFILE%\.claude\autoclaude\loop-config.json`：
```json
{
  "enabled": false,
  "maxAutoTurns": 10,
  "continuePrompt": "继续下一步。如果任务已全部完成，请在回复中输出 [[ALL_DONE]]。",
  "doneMarker": "[[ALL_DONE]]",
  "onApiError": "stop",
  "scope": "all",
  "projectFilter": ""
}
```
- `enabled`：总开关，默认 `false`（安全）。为 false 时 hook 立即放行。
- `maxAutoTurns`：硬性上限，防跑飞。任一会话累计自动续跑到此数即强制停止。
- `continuePrompt`：每轮注入的提示词（即 block 的 reason）。
- `doneMarker`：完成标记；出现在最后一条 assistant 消息里即停止。
- `onApiError`：`"stop"` | `"retry"`；决定 api_error 后是否续。（Stop hook 本身在正常
  结束时触发；此字段供未来 error 场景与 UI 语义统一，MVP 先实现 stop。）
- `scope`：`"all"` | `"project"`；`project` 时只对 `projectFilter` 路径匹配的会话生效。

### 3.2 `loop-state.json`（hook 读写，按 sessionId 记账）
```json
{
  "sessions": {
    "<session_id>": { "autoTurns": 3, "lastTurnAtMs": 0, "lastPrompt": "..." }
  }
}
```
- 每次 hook 决定"继续"，对该 session 的 `autoTurns` +1。
- `end_turn` 是"停止意图"，续跑后计数增长；达到 `maxAutoTurns` 即停并清 0（下次任务重新
  计数）。用户手动在 TUI 里输入新指令时，计数如何重置见 §6 开放问题。

### 3.3 `stop-flag`（AutoClaude 写，hook 读后删）
`%USERPROFILE%\.claude\autoclaude\stop-flag`：存在即"下一轮放行"。可全局或按 sessionId：
文件名 `stop-flag`（全局）或 `stop-<session_id>.flag`（单会话）。hook 读到后删除并放行。

### 3.4 IPC 广播帧（sensor → AutoClaude，命名管道 `\\.\pipe\autoclaude-status`）
一行 JSON，UTF-8，`\n` 结尾：
```json
{
  "sessionId": "42cfc7e0...",
  "cwd": "D:\\Razer\\TJF\\Github\\AutoClaude",
  "transcriptPath": "...\\42cfc7e0.jsonl",
  "loopEnabled": true,
  "autoTurns": 3,
  "tsMs": 0
}
```
（sensor 从 statusLine stdin JSON + `loop-state.json` 组装。`tsMs` 由接收端补，避免脚本
里用不可用的时间源。）

## 4. AutoClaude（C++）改动

### 4.1 Config（config.cpp/h）
新增字段镜像 `loop-config.json` 的可视部分，或直接读写 `~/.claude/autoclaude/loop-config.json`
（推荐后者：单一数据源，hook 与 UI 共用）。MVP 决策：**AutoClaude 直接读写
`loop-config.json`**，不进 `config.json`，避免两份配置漂移。

### 4.2 IPC 服务端（新增 `src/ipc/status_pipe.{h,cpp}`）
- worker 线程（独立于 TranscriptWatcher）`CreateNamedPipeW("\\\\.\\pipe\\autoclaude-status",
  PIPE_ACCESS_INBOUND, message/byte, 多实例)`。
- 收到一帧解析后 `PostMessage(WM_APP_LOOPSTATUS, LPARAM=LoopStatus*)` 给 UI。
- 关闭时 `CancelIoEx` + `DisconnectNamedPipe`，与现有 `Stop()` 生命周期对齐。

### 4.3 app.h 新增
```cpp
#define WM_APP_LOOPSTATUS (WM_APP + 4)  // LPARAM = LoopStatus*（UI 线程 delete）
struct LoopStatus {
    std::wstring sessionId, cwd;
    bool loopEnabled = false;
    int  autoTurns = 0;
};
```

### 4.4 UI（window.cpp）
- SessionRow 增加 `int autoTurns; bool loopEnabled;`，底行追加 "loop 3/10" 徽标。
- 新增一个"Stop Loop"按钮（或复用现有按钮布局多加一枚）：点击写 `stop-flag`。
- 循环开关（enabled）可放 Help 之外的一个 pill / toggle；MVP 可先只读显示，enabled 通过
  编辑 `loop-config.json` 或命令行开启，减少首版 UI 工作量。见 §6。

## 5. Stop hook 脚本（`autoclaude-loop.js`）逻辑

```
输入: stdin JSON { session_id, transcript_path, last_assistant_message, cwd, ... }
1. 读 loop-config.json；若 !enabled → 输出空, exit 0（放行）
2. scope=="project" 且 cwd 不匹配 projectFilter → 放行
3. 若 stop-flag 或 stop-<session_id>.flag 存在 → 删除文件, 放行
4. 若 last_assistant_message 含 doneMarker → 放行（清该 session 计数）
5. 读 loop-state.json 取该 session autoTurns；若 >= maxAutoTurns → 放行（清计数）
6. 否则：autoTurns+1 写回 loop-state.json；
   输出 {"decision":"block","reason": continuePrompt}, exit 0
```
- 所有文件 IO 包 try/catch，任何异常→放行（fail-open，绝不把用户会话卡死）。
- 用 `last_assistant_message`（hook 直接给），不读 transcript，避免异步写入滞后。

## 6. 安全与开放问题

安全：
- `enabled` 默认 false；`maxAutoTurns` 硬上限；hook 全程 fail-open。
- doneMarker 让 Claude 能自我终止循环。
- stop-flag 让用户随时喊停（AutoClaude 按钮或手动建文件）。

开放问题（实现时定，不阻塞本 spec）：
1. **计数重置**：用户在 TUI 手动输入新指令后，autoTurns 是否清零？倾向：hook 无法区分
   "自动续" vs "人工输入"触发的 turn，MVP 用 `last_assistant_message` 是否等于上轮注入
   prompt 的间接判断，或干脆每次 UserPromptSubmit hook 清零（需再加一个 hook）。
2. **enabled 的 UI**：MVP 是否做可视 toggle，还是先只读显示 + 手动改配置。
3. **多会话并发**：loop-state.json 的并发写（多个 Claude 实例同时 Stop）需文件锁或
   原子写（写临时文件 + rename）。

## 7. 验证计划

- **hook 单元**：喂各种 stdin JSON（enabled off / doneMarker 命中 / 超上限 / stop-flag /
  正常续），断言 stdout JSON 与 exit code 正确，覆盖 fail-open。
- **IPC 端到端**：起 AutoClaude → 手动往命名管道写一帧 → UI 行出现 "loop N/M"。
- **真实集成**：配好 hook，跑一个真实小任务，观察 Claude 自动续跑到 doneMarker 或
  maxAutoTurns 停止；中途点"Stop Loop"能在下一轮停下。

## 8. 分期

- **阶段 1（核心闭环）**：`autoclaude-loop.js` Stop hook + `loop-config.json` +
  `loop-state.json` + stop-flag。**不依赖 AutoClaude 即可自动接力**。含单元验证。
- **阶段 2（观测）**：sensor 广播（扩展 statusline.js）+ AutoClaude IPC 服务端 + UI 显示
  "loop N/M"。
- **阶段 3（干预 UI）**：AutoClaude "Stop Loop" 按钮 + enabled toggle。
