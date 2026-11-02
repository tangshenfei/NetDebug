#ifndef ND_GUI_H
#define ND_GUI_H

typedef void (*nd_gui_stop_fn)(void);

/* 显示本地控制窗口；窗口关闭或点击“停止服务并退出”后返回 0。
 * 非 Windows 或创建窗口失败返回 -1，调用方可回退到控制台模式。 */
int nd_gui_run(const char *url, const char *log_dir, nd_gui_stop_fn stop);
int nd_gui_capture(const char *url, const char *log_dir, const char *bmp_path);
/* 已有 GUI 实例存在时，激活它的服务面板；成功返回 1。 */
int nd_gui_activate(void);

#endif
