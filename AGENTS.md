# AGENTS.md — kill-process-type 开发指南（AI 助手 / 新成员必读）

## 项目是什么

纯 C（Win32 API）的轻量 Windows 桌面工具：查看并终止 Node.js / Python 进程、
查看全部进程、查看端口占用。单文件 exe、零第三方依赖、带托盘驻留与开机自启。

## 常用命令

| 操作 | 命令（在 `kill-process-type/` 目录下） |
|---|---|
| 构建（首选） | `build.bat`（双击或命令行；构建前先退出运行中的实例） |
| 构建（备选） | `mingw32-make` |
| 模块测试 | `mingw32-make test` |
| UI 自动化测试 | `powershell -NoProfile -ExecutionPolicy Bypass -File tests\ui-flow-test.ps1`（先 `gcc -O2 -o build\clipread.exe tests\clipread.c`） |
| 重新生成图标 | `powershell -NoProfile -ExecutionPolicy Bypass -File res\make-icon.ps1` |

构建产物：`build\kill-process-type.exe`（约 100KB，`-static` 无运行库依赖，可直接单文件分发）。

## 架构（src/）

| 文件 | 职责 |
|---|---|
| `main.c` | 入口：单实例互斥、公共控件初始化、`/tray` 静默启动参数、消息循环 |
| `app.h` | 全局 `App g_app` 状态结构（控件句柄/模式/数据缓存）+ 模式枚举 + `PortRow`，**跨模块共享状态唯一入口** |
| `gui.c` | 主窗口外壳：控件创建、布局、字体、页签、消息路由 |
| `views.c` | 视图渲染：列定义、筛选匹配、快照→行填充、勾选跨刷新保持、状态栏统计 |
| `actions.c` | 用户动作：终止进程（全部类型/勾选）、复制路径、列表右键菜单 |
| `process.c` | 进程枚举（node/python 识别 `ClassifyName`、全进程）、`TerminateProcess` 封装 |
| `net.c` | 端口监听扫描（GetExtendedTcpTable/UDP，双栈）+ 系统保留端口区间（netsh excludedportrange 解析） |
| `ai.c` | AI 风险评估：无头调用 kilo run `--format json --thinking`（reasoning/text 事件分离解析 + 3 次退避重试 + stderr 诊断，路径回退链 KILO_EXE→仓库→D:\kilo→PATH） |
| `richtext.c` | Rich Edit 渲染：Markdown 子集→RTF（注意 RTF 字体必须 \fcharset134，否则中文乱码） |
| `cli.c` | 无头 CLI：/list /ports /kill，UTF-8 JSON 到 stdout（供 AI 工具链调用） |
| `config.c/.h` | 配置持久化（INI：exe 目录优先，%APPDATA% 回退；Profile API 直读直写） |
| `theme.c/.h` | 深色主题（DWM 标题栏/ListView/表头自绘/Tab 子类化+OWNERDRAW/按钮 ownerdraw；浅色零侵入走系统默认） |
| `settings.c/.h` | 设置窗口：自启/启动最小化/主题/自动刷新/气泡通知，全部实时生效 |
| `tray.c` | 托盘图标、托盘菜单（含设置入口、自启开关）、气泡通知（受 BalloonNotify 配置控制） |
| `startup.c` | 开机自启动（HKCU Run 注册表读写） |

**模块规则**：
- 新增功能先确定归属：纯数据逻辑 → process/net；列表怎么显示 → views；用户操作 → actions；窗口/控件/路由 → gui。
- 控件句柄与状态一律走 `g_app`，禁止新增文件级 static 副本。
- 视图模式新增时：`app.h` 加枚举 + `views.c` 加列定义与行渲染分支 + `gui.c` 页签数组加一项，三处同步。

## 硬性约定（全部踩过坑，勿违反）

1. **`.bat` / `.ps1` 必须纯 ASCII + CRLF**：中文注释在 GBK 码页下会导致
   `'xx' 不是内部或外部命令` / 吞换行 / 解析错乱。中文只能写在 `.c/.h/.md` 里。
2. **`res/manifest.xml` 不得包含 `<compatibility>/<supportedOS>` 段**：本机 SxS
   加载器会拒绝并报 side-by-side 错误（windres 2.30/2.42 均复现；该段仅影响
   GetVersion 兼容性，已移除无损失）。
3. **windres 用 Strawberry 2.42**：`C:\Strawberry\c\bin\windres.exe`（build.bat
   已内置存在性回退）。
4. **含中文的 UI 一律在 C 源码运行时构建**（`AppendMenuW` 等），不要放 `.rc`
   （windres 编码风险）。
5. **`WIN32_LEAN_AND_MEAN` 排除了 `shellapi.h`**：用托盘/SHGetFileInfo 需显式
   `#include <shellapi.h>`。
6. **禁止跨进程 `SendMessage` 携带指针参数的 `LVM_*` 消息**（如 LVM_SETITEMSTATE
   传 LVITEM 指针）：本机 comctl32 跨进程封送会访问冲突崩溃。UI 自动化必须走
   应用内路径：`PostMessage(WM_CONTEXTMENU/WM_COMMAND)` 或真实输入注入；
   无指针的消息（LVM_GETITEMCOUNT、TCM_GETCURSEL 等）可安全 SendMessage。
7. **剪贴板断言不要依赖 PowerShell Get-Clipboard**（本机偶发 OLE 剪贴板被锁，
   Set/Get-Clipboard 抛 `Requested Clipboard operation did not succeed`）：
   用 `tests/clipread.c` 独立进程读。
 8. 命名遵循项目规范：文件 kebab-case、变量/函数 camelCase、类型 PascalCase；
    用户可见字符串一律宽字符 `L""`。
 9. 格式化字符串只用 `%d / %lu / %ls / %02d` 等基础项（strsafe/老 msvcrt 兼容），
   禁 `%f`（内存用整数 MB+小数位手工拼）。
10. **自动化读 ListView 行文本用 MSAA**（oleacc `AccessibleObjectFromWindow`，
    子项从 1 起 `accName(i)`）：本环境 UIA 对该控件只暴露 Pane 读不到行；
    PS 里 `OBJID_CLIENT` 常量需写 `[uint32]4294967292`（0xFFFFFFFC 会被当负数）。
11. **禁止用 `Get-Content`/`Set-Content` 改写 `.c/.h` 文件**：PS5.1 按 GBK 误读
    UTF-8 无 BOM 源码，中文注释/字符串会被不可逆损坏。必须用
    `[System.IO.File]::ReadAllText/WriteAllText`（自动按 UTF-8 处理）或编辑工具。
12. **带 `LVS_EX_DOUBLEBUFFER` 的 ListView，`LVM_GETBKCOLOR` 返回内部垃圾值**：
    断言主题色用 `LVM_GETTEXTCOLOR`（0x1023）或像素取样，勿信 GETBKCOLOR。
13. **深色主题相关**：DWMWA_USE_IMMERSIVE_DARK_MODE 值 20（20H1+）/19（1809）；
    表头深色须 NM_CUSTOMDRAW 全自绘（NMCUSTOMDRAW 无颜色字段，须自绘含排序箭头）；
    DRAWITEMSTRUCT 字段是 `hDC`、NMCUSTOMDRAW 是 `hdc`（大小写不同，别搞混）；
    **处理 NM_CUSTOMDRAW 必须先验证 hwndFrom 是 SysHeader32**——ListView 自身
    绘制项时也发该通知，误劫持返回 SKIPDEFAULT 会导致整列表空白不绘制。

## 测试纪律

- 改 `process.c / net.c / startup.c` → 跑 `mingw32-make test`（注册表测试遵循
  零残留：结束时恢复初始状态）。
- 改 `gui.c / views.c / actions.c` → 构建后跑 `tests\ui-flow-test.ps1`（驱动
  右键复制、页签切换、列排序的真实代码路径），并手工冒烟：启动→三个页签
  切换→列头排序→托盘右键→退出。
- 改 `cli.c` → 跑 `tests\cli-test.ps1`（/list /ports /kill，JSON 必须可解析）。
- 改 `theme.c / config.c / settings.c` → 跑 `tests\theme-test.ps1`（浅/深色生效
  断言用 LVM_GETTEXTCOLOR；设置窗口可达；配置文件生成）。
- 改 `ai.c` → 跑 `tests\ai-e2e-test.ps1`（右键→AI 分析全链路，需 kilo 已登录，
  耗时约 1~2 分钟）；kilo 调用注意：横幅/日志在 stderr、答案在 stdout；
  kilo 偶发退出码 3 属间歇性故障（3 次退避重试兜底），复现条件排查用
  `tests/test_ai_spawn.c`（精确复刻应用侧调用环境）。
- 构建/覆盖 exe 前必须退出正在运行的实例（文件占用导致链接 `Permission denied`）。

## 维护本文档

以下情况必须同步更新本文件与 `DESIGN.md`：
- 新增/删除源码文件或模块职责变化；
- 发现新的"硬性约定"级踩坑；
- 构建、测试命令变化；
- 新增页签/视图模式。
