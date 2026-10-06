/* views.h - 视图渲染接口：数据缓存 → ListView 行/列/状态栏 */
#ifndef KPT_VIEWS_H
#define KPT_VIEWS_H

#include "app.h"

/* 按当前模式（页签）重建列表列头 */
void ViewsSetColumns(void);

/* 从缓存重建列表行（应用筛选框输入），不做系统快照 */
void ViewsRebuild(void);

/* 重做系统快照并重建列表（按当前模式取数） */
void ViewsRescan(void);

/* 读取页签当前选中项，切换模式（换列头/筛选提示，可选立即重扫） */
void ViewsApplyMode(BOOL rescan);

/* 列头点击排序：同列切换升降序，异列切换排序列（单列生效） */
void ViewsSortBy(int col);

/* 树形模式：切换某 PID 子树的折叠状态并重建列表 */
void ViewsToggleCollapse(DWORD pid);

/* 释放数据缓存（WM_DESTROY 调用） */
void ViewsCleanup(void);

#endif
