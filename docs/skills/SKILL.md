# kill-process-type CLI + AI 技能

> 仓库内权威副本 · 同步至 C:\Users\10456\.kilocode\skills\process-kill\SKILL.md
> 更新时间：2026-10-06 v5.2-WP4

## 基本命令（无 AI）

| 命令 | 说明 |
|---|---|
| `<exe> /list` | 全部进程 JSON |
| `<exe> /ports` | 监听+保留区间 JSON |
| `<exe> /orphans[:nodepy]` | 孤儿进程 JSON |
| `<exe> /top [node|python|all] [N]` | 按内存降序 Top N JSON（含命令行+cwd） |
| `<exe> /kill <pid> [...]` | 按 PID 终止，自动落审计日志 |

## AI 命令（v5.2 新增）

| 命令 | 说明 |
|---|---|
| `<exe> /ai-query "问题"` 或 `<exe> /ai-query @file.txt` | 自然语言查询，返回 JSON {answer, pids, analysis} |
| `<exe> /ai-suggest <port>` | 端口故障根因+修复建议 JSON {root_cause, evidence, fix_steps} |

- `/ai-query` 建议用 `@file` 方式传中文问题（规避 cmd 引号/编码截断）
- `/ai-suggest` 无 AI 时降级输出本地规则版结论（保留区间→EACCES 话术）
- kilo 不可用时退出码 2，输出 `{"error":"..."}`

## 意图分类

### 查询类（只读，安全）
- 按内存/路径/端口/项目筛选进程 → `/list` + `/top` + `/ports` 组合
- "展示 xxx 项目所有进程" → `/list` + 按 cwd 字段过滤

### 分析类（只读，调 AI）
- 端口故障 / EACCES 诊断 → `/ai-suggest <port>`
- 自然语言进程查询 → `/ai-query`

### 策略类（只读建议，不执行）
- AI 生成清理策略（v6.0 `/ai-clean-orphans`，当前未实现）

### 执行类（危险，需确认）
- 杀 PID → `/kill <pid>`：**工具内部有安全护栏**（pid 0/4、自身、路径
  不可读、Windows 目录进程自动跳过），但调用方仍须**先向用户展示 PID
  清单并确认**后才执行
- AI 永远不自主杀进程；AI 输出的 PID 必经工具护栏校验

## 安全护栏（AI 不可绕过）

1. 系统关键进程（csrss/winlogon/wininit 等路径不可读的 SYSTEM 进程）永远被拦截
2. Windows 系统目录下进程在全量模式下被跳过
3. pid 0/4 / 工具自身永远不可杀
4. 所有终止操作落审计日志（来源标记 AI-Assisted）
