# kill-process-type

轻量级 Windows 进程管理小工具（纯 C + Win32 API，单文件 exe 约 178KB，零第三方依赖）。

## 功能

- **进程管理**：全部进程 / Node-Python 专视 / 端口占用三页签，列排序、筛选、图标（与任务管理器一致）
- **进程终止**：按类型/勾选/PID 终止，失败逐条给出原因，批量二次确认
- **孤儿进程定时清理**：定时扫描父进程已退出的残留进程（默认仅 Node/Python）并自动释放，也可托盘手动触发；多重安全护栏（跳过系统进程/路径不可读进程/Windows 目录）
- **端口诊断**：TCP/UDP 双栈监听列表 + **winnat 保留端口区间**（定位 `listen EACCES`），右键一键提权修复（UAC）
- **AI 风险评估**（kilo 集成）：右键任意进程 → 提示词/思考过程/分析结果三段式展示（Markdown 渲染），终止前风险评估
- **无头 CLI**：`/list` `/ports` `/orphans` `/kill <pid>` 输出 UTF-8 JSON，可被 AI 工具链直接调用
- **托盘**：常驻、快捷菜单、气泡通知、开机自启
- **深色主题**：跟随系统/浅色/深色三态热切换
- **设置**：INI 配置持久化、窗口位置记忆、自动刷新可配

## 构建

依赖：MinGW-w64（gcc + windres，Strawberry windres 2.42 优先）

```bat
cd kill-process-type
build.bat
```

产物：`build\kill-process-type.exe`（`-static` 无运行库依赖，可直接单文件分发）

## 文档

- [DESIGN.md](DESIGN.md) — 设计文档（架构 / 模块 / 踩坑记录 / 更新日志）
- [AGENTS.md](AGENTS.md) — 开发指南（硬性约定 / 测试纪律 / 模块职责）

## 测试

```bat
mingw32-make test                                :: 模块测试
tests\ui-flow-test.ps1                           :: UI 自动化（复制/页签/排序）
tests\cli-test.ps1                               :: 无头 CLI（JSON 校验）
```
