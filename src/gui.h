/* gui.h - 主窗口外壳接口 */
#ifndef KPT_GUI_H
#define KPT_GUI_H

#include "app.h"

/* 注册窗口类并创建主窗口（含控件、页签、托盘与初始刷新） */
BOOL GuiCreateMain(HINSTANCE hInst, int nCmdShow);

/* 设置变更后重应用：主题热切换 + 自动刷新定时器重配置 */
void GuiApplySettings(void);

#endif
