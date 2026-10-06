# kill-process-type 设计文档

> 版本：v4.0（2026-09-11，移植 memreduct：配置系统/设置窗口/深色主题/自启增强）
> 配套开发指南见 [AGENTS.md](AGENTS.md)

轻量级 Windows 小工具：查看并终止 Node.js / Python 进程、浏览全部系统进程、
查看端口占用与 winnat 保留区间（EACCES 诊断）、**AI 杀进程风险评估（kilo）**、
**无头 CLI 供 AI 工具链反向调用**，托盘常驻、支持开机自启。

## 1. 目标与约束

| 项 | 说明 |
|---|---|
| 语言 | C（C17），纯 Win32 SDK，零第三方依赖 |
| 产物 | 单文件 `kill-process-type.exe`，约 100KB，`-static` 无运行库 |
| 权限 | 普通用户即可；高权限进程终止失败时逐条给出原因 |
| 兼容 | Windows 7+（64 位编译） |

## 2. 技术选型

- **GUI**：`CreateWindowExW` 原生窗口 + ComCtl32 v6（ListView / Tab / StatusBar），
  manifest 启用视觉样式与 PerMonitorV2 DPI。
- **进程枚举**：`CreateToolhelp32Snapshot`；路径 `QueryFullProcessImageNameW`；
  内存 `GetProcessMemoryInfo`。
- **进程终止**：`OpenProcess(PROCESS_TERMINATE)` + `TerminateProcess`。
- **端口扫描**：`GetExtendedTcpTable / GetExtendedUdpTable`（双栈，见 net.c）。
- **进程图标**：`SHGetFileInfoW(SHGFI_SYSICONINDEX)` 挂接系统图像列表，
  与任务管理器一致、零额外内存。
- **托盘**：`Shell_NotifyIconW` + 运行时 `AppendMenuW` 菜单。
- **编译**：MinGW-w64 gcc（本机 8.1）+ windres（Strawberry 2.42，见踩坑记录）。

## 3. 架构与模块划分

```
src/
├── main.c        入口：单实例互斥、InitCommonControls、/tray 参数、消息循环
├── app.h         全局 App g_app（句柄/模式/缓存）+ ListMode 枚举 + PortRow
├── gui.c/.h      窗口外壳：控件创建、布局、字体、页签、消息路由
├── views.c/.h    视图渲染：列定义、筛选、快照→行、勾选保持、状态栏统计
├── actions.c/.h  用户动作：终止进程、复制路径、列表右键菜单
├── process.c/.h  进程枚举（node/python 识别、全进程）与终止
├── net.c/.h      端口监听扫描（TCP/UDP × IPv4/IPv6）+ 系统保留区间
├── ai.c/.h       AI 风险评估：无头调用 kilo run（管道捕获 + 线程 + 超时）
├── cli.c/.h      无头 CLI（/list /ports /kill，UTF-8 JSON 输出）
├── tray.c/.h     托盘图标/菜单/气泡
├── startup.c/.h  开机自启动（HKCU Run）
├── common.h      UNICODE/_WIN32_WINNT/PSAPI_VERSION 等统一编译环境
└── resource.h    资源与命令 ID
```

依赖方向：`gui → views/actions → process/net → common`；
`app.h` 为共享状态唯一定义点，禁止跨模块 static 副本。

## 4. UI 设计

```
+------------------------------------------------------------------+
| [刷新] [杀死选中] [杀死全部Node] [杀死全部Python]  [✓自动刷新(10s)] |
+------------------------------------------------------------------+
| [全部进程][Node/Python][端口占用]          [筛选进程名，如：node]   |
+------------------------------------------------------------------+
| □ | 图标 进程名        | PID   | 父PID | 内存    | 类型   | 路径   |
| □ | 🐍 python.exe      | 12346 | 900   | 24.1MB | Python | D:\... |
|   | ...                                                            |
+------------------------------------------------------------------+
| Node.js: 2 个    Python: 1 个    上次刷新 21:35:07                |
+------------------------------------------------------------------+
```

- **页签**（WC_TABCONTROL，左侧）：三个视图共用一套 ListView，切换即换列头
  与数据源；筛选框右对齐同一行（任务管理器式布局）。
- **全部进程**：系统全量进程（`ScanAllProcesses`），类型列标注 Node.js /
  Python / —，同样支持筛选、勾选终止、右键复制路径。
- **Node/Python**：仅目标类型（核心场景）。
- **端口占用**：端口/协议/PID/进程名/类型/内存/路径，与全进程快照 Join，
  进程已退出显示占位符。
- **系统保留端口区间（EACCES 诊断）**：端口视图追加 winnat / Hyper-V 动态
  保留与管理员保留区间行（`netsh excludedportrange` 四路解析，无控制台闪窗，
  无需管理员）。此类端口 `listen` 报 `EACCES` 且**没有进程可杀**：
  - 行显示 `8777-8876 TCP (系统保留端口区间)`，筛选端口命中区间即显示；
  - 右键该行可**一键修复**：工具内 `ShellExecuteExW(runas)` 拉起提权 cmd
    自动执行 `net stop/start winnat` → `netsh add excludedportrange
    store=persistent`（固定目标端口）→ 验证输出，仅一次 UAC 确认，全程免复制；
    目标端口取筛选框中落在区间内的值，否则用区间起始端口；
  - 右键另提供**仅复制修复命令**（无 UAC 场景的备用路径）；
  - 误对保留行"杀死选中"时弹窗解释原因，可直接选择"是"进入一键修复；
  - 排序分层：正常监听 < 保留区间 < 已退出进程。
- **筛选**：进程模式为名称子串（不区分大小写）；端口模式支持
  `3000`、`80,443`、`3000-3010` 组合；输入即筛选（仅重绘不重扫）。
- **列排序**：点击列头即按该列排序（单列生效），再点同列切换升/降序，
  表头显示 ▲/▼ 箭头；排序在自动刷新后保持；切换页签时重置（列集不同）。
  端口视图中"进程已退出"行恒排最后，不受方向影响；同键以 PID 兜底保证
  排序结果确定。
- **勾选保持**：自动刷新按 PID 保存/回放勾选，不会因列表重建丢失。
- **行右键**：「复制可执行路径」（多选行以 CRLF 连接；路径不可读时回退进程名），
  支持 Shift+F10 键盘触发。
- 关闭(X) → 隐藏到托盘（气泡提示一次）；10s 自动刷新仅可见时执行。

### 托盘

左键单击切换主窗口显隐；右键菜单：

```
刷新进程列表
杀死全部 Node.js
杀死全部 Python
────────────────
☑ 开机自启动        （HKCU Run，值 = "exe路径" /tray）
显示 / 隐藏主界面
退出
```

杀进程后气泡反馈结果；批量操作前二次确认。

## 5. 关键实现要点

1. 全 Unicode（`-municode` + W 系列 API）；DPI 缩放统一 `AppScale()`
   （`WM_DPICHANGED` 时更新字体与布局）。
2. 单实例：`CreateMutexW(Local\\kill-process-type-single)`，二次启动激活已有窗口。
3. `/tray` 参数：登录自启后静默驻留托盘（气泡提示一次）。
4. 端口视图按 PID 去重终止（同一进程多端口占多行）。
5. 剪贴板：`CF_UNICODETEXT`，成功后内存归剪贴板，失败路径 `GlobalFree`。
6. 列排序在数据层完成（`qsort` 缓存数组后重建行），而非 `ListView_SortItems`，
   便于刷新后保持顺序；箭头通过 `HDITEM.fmt` 的 `HDF_SORTUP/HDF_SORTDOWN`。

## 6. 构建与测试

```bat
build.bat          :: 首选；构建前退出运行中的实例
mingw32-make       :: 备选
mingw32-make test  :: process/net/startup 模块测试（注册表零残留）
powershell -NoProfile -ExecutionPolicy Bypass -File tests\ui-flow-test.ps1
                   :: UI 自动化（右键复制 + 页签切换，走应用内消息路径）
```

UI 测试需先编译剪贴板读取器：`gcc -O2 -o build\clipread.exe tests\clipread.c`。

## 7. 踩坑记录（实测结论，写入 AGENTS.md 硬性约定）

1. manifest 含 `<compatibility>/<supportedOS>` 段 → SxS 启动失败（windres
   2.30/2.42 均复现），已移除。
2. `WIN32_LEAN_AND_MEAN` 排除 `shellapi.h`，托盘 API 需显式包含。
3. `.bat`/`.ps1` 中文注释在 GBK 码页下解析错乱，必须纯 ASCII + CRLF。
4. windres 内嵌中文资源有编码风险：中文 UI 一律 C 源码运行时构建。
5. **跨进程 SendMessage 传指针的 LVM_* 消息（如 LVM_SETITEMSTATE+LVITEM）会
   使 comctl32 访问冲突崩溃**（本机 Win11 22621 复现）；UI 自动化须走
   PostMessage(WM_CONTEXTMENU/WM_COMMAND) 等无指针路径。
6. 本机 PowerShell OLE 剪贴板偶发被锁（Get/Set-Clipboard 抛异常），剪贴板
   断言用独立进程 `clipread.exe`。
7. 本环境 UIA 对该 ListView 只暴露 Pane（读不到行）；自动化读行文本用
   MSAA（oleacc `AccessibleObjectFromWindow`，子项 1 起 `accName(i)`）。

## 8. 更新日志

- **v6.1（2026-10-06）**：P2 全部三项 + P1 对话面板框架——
  **WP11 时序异常告警**——MonitorStart 启动采样线程；自动刷新时注册
  node/python PID 到时序监控（AnomalyWatch 配置开关）；ActionsAnomalyCheck
  检测内存持续增长（6 样本连续上升 + 增速>10MB/12s≈50MB/min）→ 托盘
  气泡告警（绝不自动杀）；
  **WP12 基线对比**——托盘菜单「保存基线快照」（进程+端口清单→
  baseline.txt）与「与基线对比」（差异报告：新增/消失进程+新增/消失
  端口四类 + 汇总统计，MessageBox 展示）；
  **WP7 AI 对话面板框架**——ChatPanelToggle 可折叠侧边面板（360px
  RichEdit 会话区+输入框+发送按钮），ChatProc 消息处理与主题适配就位，
  AI 调用集成标记 TODO（后续精化）。
- **v6.0.1（2026-10-06）**：v6.0 补全——
  **WP13 智能重启**——任意进程右键新增「智能重启（杀后原参数拉起）」：
  PEB 采集原命令行+工作目录 → 确认弹窗 → TerminateProcess →
  CreateProcessW 以原参数+原 cwd 重新启动 → 气泡通知新旧 PID 映射；
  重启失败弹窗提示原命令行供手动恢复；审计日志来源=智能重启；
  **WP9 AI 清理策略**——托盘菜单新增「AI 清理策略」：扫描全量孤儿 →
  kilo 三档分级（✅auto=安全自动清理 / ⚠️manual=需人工复核 /
  ❌forbid=禁止操作）→ AI 报告窗口展示策略清单 → 「清理 auto 项」
  按钮一键执行安全项（护栏兜底：系统进程始终拦截）。
- **v6.0（2026-10-06）**：v6.0 两项——
  **WP8 项目分组 UI**——Node/Python 页签新增「项目」复选框（持久化
  ProjectView 配置）；快照采集时对 node/python 进程调 GetProcessProject
  填充 ProcInfo.project；项目分组模式：按项目根分组渲染（项目根行
  ▾ 名称+计数 → 子进程缩进行），未识别项目单列；列表显示命令行列。
  **WP10 AI 推荐配置**——设置窗口新增「AI 推荐」区块与按钮；采集
  统计（终止日志总数/孤儿清理次数/失败次数/当前配置快照）→ kilo
  分析 → AI 报告窗口展示推荐清单（JSON 格式 key/value/reason）。
  同时 v5.3 底座已含 WP6 monitor.c/h（时序采集）+ WP8 project.c/h。
- **v5.3（2026-10-06）**：v5.2 修补 + v6.0 底座——
  **/ai-query 上下文修复**（按内存降序取前 30，优先含 node/python）；
  **诊断动作按钮**（解析 ACTIONS: JSON → 动态创建 clean_orphans/
  fix_winnat 按钮，最多 5 个）；
  **WP6 monitor.c/h**（时序采集底座：环形缓冲 MONITOR_SLOTS=60 × 2s =
  2 分钟窗口，MONITOR_MAX_PIDS=64 LRU 淘汰；独立采样线程 GetProcessTimes
  差分 CPU% + GetProcessMemoryInfo 工作集；MonitorStart/Stop/Add/Remove/
  GetSeries API，P2 异常监控的地基）；
  **WP8 project.c/h**（项目根识别：FindProjectRoot 从 PEB cwd 向上 3 级
  探测 package.json/pyproject.toml/requirements.txt；GetProcessProject
  按 PID 取项目归属；纯本地无 AI）。
- **v5.2.1（2026-10-06）**：v5.2 收尾——WP5 核心功能补全
  （DiagBuildContext 六源快照组装→kilo→四区块报告渲染→导出 .md，
  GetSaveFileNameW 对话框+UTF-8 写盘）；诊断结果路由
  （s_diagPending→WM_APP_AI_DONE→DiagApplyResult）；WP3/WP2/WP5 按钮
  初始可见性修复（WS_VISIBLE+默认页签显示，切页签动态切换）。
  **E2E 验证**：WP4 /ai-suggest 85s + /ai-query @file 45s 实际 kilo 调用
  通过（JSON 输出可 ConvertFrom-Json 解析）；WP2/WP3/WP5 按钮存在性与
  可见性经 EnumChildWindows 验证正确（2011/2012/2013，默认页签对应
  显示/隐藏状态无误）。
- **v5.2-WP3+WP5（2026-10-06）**：
  **WP3 批量风险扫描**——RiskLevel 枚举 + ProcInfo.aiRisk 字段；勾选≤50 进程
  → PROMPT_RISK_BATCH（单次调用紧凑 TSV 上下文）→ AiExtractJson 解析
  [{pid,level}] 回填；新增「AI风险」列（NM_CUSTOMDRAW 子项着色：高红/
  中黄/低绿/—灰）；「AI 风险扫描」按钮仅进程视图可见；
  **WP5 AI 诊断页签**——第 5 页签「AI 诊断」+ ActionsAiDiagOpen 独立窗口
  （生成按钮+进度状态+RichEdit 报告区+导出按钮）；MODE_DIAG 无列表列/
  无快照，诊断窗口带主题适配；完整快照组装+四区块报告+动作按钮解析
  为后续迭代精化（当前框架就位，kilo 调用链路 TODO）。
  修复：process.h↔app.h 循环包含（RiskLevel 移入 process.h）。
- **v5.2-WP2+WP4（2026-10-06）**：
  **WP2 日志复盘**——日志页签新增「AI 复盘日志」按钮（切到日志页签时
  出现），勾选日志行→PROMPT_LOG_REVIEW→AI 报告窗口（复用评估对话框机制，
  无终止按钮），上限 100 条防上下文过载，超限截断标注；
  **WP4 CLI AI 接口**——`/ai-query "问题"` 或 `@file.txt` 文件入参
  （规避中文/引号截断），同步调 kilo（CLI 阻塞可接受），AiExtractJson
  结构化提取；`/ai-suggest <port>` 端口故障分析，无 AI 时降级本地规则
  版（保留区间→EACCES 话术，进程占用→kill 建议）；kilo 技能文档仓库内
  副本 docs/skills/SKILL.md + 全局目录同步（四类意图：查询/分析/策略/
  执行，执行类强制人工确认）。
- **v5.2-WP1（2026-10-06）**：AI 核心层扩展（推敲定稿 v2.0 首个工作包）——
  6 套 Prompt 模板（AiPromptId：单风险/批量分级/日志复盘/全局诊断/CLI 查询/
  端口分析）；AiExtractJson 结构化提取器（括号配对+字符串感知，围栏/裸输出
  兼容）；AiTruncateContext 上下文限流（24KB 整行截断+标注）；
  WM_APP_AI_PROGRESS 批量进度消息预留。12/12 单测全过。
- **v5.1（2026-10-06）**：树形模式支持列排序——兄弟节点按当前排序列排序，
  **内存列 = 子树合计**（进程组占用总量，有子节点时内存列显示合计值，
  首次点击默认降序：最大分组在前）；根节点列表同规则排序；状态栏提示
  「内存列=子树合计」。重构树渲染为父子链表 + 显式栈 DFS。
- **v5.0（2026-10-06）**：Node.js/服务端开发辅助功能——
  **进程树分组**（全部进程视图「树形」开关：按父子链缩进 + 双击行
  折叠/展开子树，持久化 TreeView 配置）；
  **Dev 快捷右键**（端口行：浏览器打开/复制 URL；进程行：复制完整命令行、
  在终端打开所在目录、在资源管理器中显示）；
  **PEB 命令行/工作目录**（peb.c：NtQueryInformationProcess 读
  RTL_USER_PROCESS_PARAMETERS，x64 偏移 0x70/0x38；Node/Python 视图新增
  「命令行」列，快照时对 node/python 采集）；
  **CLI /top**（按内存降序输出前 N 个进程 JSON，含完整命令行+工作目录）；
  未实现项存档至 `../docs/2026-10-06-094029-kill-process-type-功能规划.md`。
- **v4.2.1（2026-10-06）**：修复日志页签闪退——KlogLoad 头部插入的 memmove
  参数写错（`&items[idx+1],&items[idx]` 应为整体右移 `&items[1],&items[0]`），
  导致按记录数平方倍的越界读写（220 条时 ~163KB 堆损坏 0xC0000374）；
  小数据量（2~3 条）越界落在堆余量内不崩，故此前测试未暴露。改为
  文件序追加+末尾反转（同时消除 O(n²)）；新增 tests\test_klog.c 压测工具
  （300 轮真实日志加载回归）。
- **v4.2（2026-09-25）**：终止进程日志——所有清理路径（勾选/类型/孤儿定时/
  孤儿手动/AI 评估/CLI）逐条落盘（时间/来源/名称/PID/结果/错误码/路径，
  UTF-8 BOM 制表符分隔，与配置同目录，>512KB 滚动 .old，Excel 可直接打开）；
  主窗口新增第 4 页签「日志」：列排序、按名称/来源/路径筛选、右键复制路径、
  状态栏成功/失败统计；KillPids 增加 per-PID 错误码输出供日志使用。
- **v4.1（2026-09-25）**：孤儿进程定时清理（参考 memreduct 定时自动清理模式）——
  孤儿判定 = 父进程已退出仍存活（ppid 不在存活集）；定时静默清理（默认仅
  Node/Python，间隔 1~1440 分钟可配）+ 托盘手动清理（清单确认）；安全护栏：
  跳过 pid 0/4、ppid==0、本进程、路径不可读（SYSTEM 会话进程如 csrss）、
  Windows 目录进程；新增 CLI `/orphans[:nodepy]`（JSON，供 AI 工具链）；
  设置窗口新增「孤儿进程清理」区块（开关/间隔/范围）。
- **v4.0（2026-09-11）**：移植 memreduct 三大功能——
  **配置系统**（config.c：INI 持久化，exe 目录便携优先/%APPDATA% 回退，
  键含 AutoRefresh/Interval/StartMinimized/Theme/BalloonNotify/窗口位置）；
  **设置窗口**（settings.c：自启/启动最小化/主题三态/自动刷新间隔/气泡开关，
  改动实时生效，托盘菜单"设置..."入口）；
  **深色主题**（theme.c：跟随系统 AppsUseLightTheme + 手动浅/深三态热切换、
  DWM 沉浸式标题栏、ListView/表头/状态栏/Tab/按钮全面深色化、
  WM_SETTINGCHANGE 系统切换热响应；浅色模式零侵入）；
  **主题变更全局广播**：设置窗口切换或系统自动切换时，主窗口/设置窗口/
  AI 评估窗口三方联动实时重应用；
  **多屏 DPI 健壮性**：设置窗口独立 DPI 缩放 + WM_DPICHANGED 重排、
  弹窗锚定主窗口位置（避免 CW_USEDEFAULT 漂移到异 DPI 屏幕）；
  动态窗口按钮保持系统样式（动态窗口场景 ownerdraw 实测不生效）。
  **自启增强**（启动时最小化静默驻留，区别于 /tray 带气泡）；
  主窗口位置记忆。踩坑：DOUBLEBUFFER 下 LVM_GETBKCOLOR 不可信（改用
  GETTEXTCOLOR/像素断言）；表头深色须 NM_CUSTOMDRAW 全自绘。
- **v3.3（2026-09-11）**：AI 评估窗口等待计时——分析中状态栏每秒刷新
  「已等待 X 秒/X 分 X 秒」，完成/失败后显示总用时；修复非端口视图下
  PID=0 行（[System Process]）被误判为保留区间行的问题。
- **v3.2（2026-09-11）**：AI 评估窗口三段式展示（提示词/思考过程/分析结果）——
  kilo 改用 `--format json --thinking` 事件流（reasoning/text 分离解析，含
  \uXXXX 与代理对反转义、非事件输出兜底）；结果窗口换 Rich Edit
  （RICHEDIT50W），自研 Markdown 子集→RTF 渲染（加粗/行内代码/标题/列表/
  颜色，GBK 字节 + \fcharset134，非 GBK 字符 \uN 回退）；失败诊断以红色段
  展示。踩坑：RTF 字体必须声明 \fcharset134，否则中文字节按 CP1252 解码
  呈现乱码。
- **v3.1（2026-09-11）**：kilo 调用健壮性——3 次退避重试（3/8/15s，
  应对 kilo 偶发退出码 3 的间歇性故障）、stderr 重定向临时文件用于失败诊断
  （对话框直接展示 kilo 退出码与错误输出）、子进程固定 USERPROFILE 工作目录、
  诊断工具 tests/test_ai_spawn.c。
- **v3.0（2026-09-11）**：与 kilo CLI 双向打通——
  **A. 工具→AI**：任意进程行右键「AI 风险评估（kilo）」，无头调用
  `kilo run`（kilo 横幅/日志走 stderr，stdout 仅答案），工作线程 + 管道捕获
  + 5 分钟超时看护；结果窗口展示分析（进程身份/风险评级/依据/建议）并支持
  一键终止。kilo 路径回退链：`KILO_EXE` 环境变量 → 本仓库发行包 →
  D:\kilo 安装位置 → PATH。实测端到端约 40~90 秒。
  **B. AI→工具**：新增无头 CLI `/list` `/ports` `/kill`（UTF-8 JSON 到
  stdout，cmd 管道可靠捕获；PowerShell 直调 GUI 子系统 exe 不可靠），
  并注册 kilo 技能 `process-kill`（全局 skills 目录），AI 助手可对话式
  管控进程、诊断 EACCES。
- **v2.3（2026-09-11）**：保留端口一键提权修复（UAC + 提权 cmd 自动执行，
  工具内闭环，免手动复制命令）；"杀死"保留行弹窗可直接选择进入修复。
- **v2.2（2026-09-11）**：端口视图新增系统保留区间诊断（netsh 解析、右键
  复制修复命令、误杀提示），定位 winnat 保留导致的 listen EACCES。
- **v2.1（2026-09-11）**：列头点击排序（单列生效、升降切换、表头箭头、
  刷新保持、切页签重置；端口视图已退出进程恒排最后）。
- **v2.0（2026-09-10）**：新增「全部进程」页签（Tab 控件替换原单选按钮）；
  模块化重构（app.h / views.c / actions.c 从 gui.c 拆出）；新增 AGENTS.md；
  UI 自动化测试改为应用内消息路径（ui-flow-test.ps1）。
- v1.2（2026-09-10）：列表行右键复制可执行路径（多选 CRLF 连接、路径缺失回
  退进程名、支持键盘触发）。
- v1.1（2026-09-05）：开机自启动（托盘开关 + HKCU Run + /tray 静默启动）；
  进程图标（系统图像列表，与任务管理器一致）。
- v1.0（2026-09-05）：初版：Node/Python 枚举与终止、托盘、端口占用视图、
  筛选、自动刷新、单实例。
