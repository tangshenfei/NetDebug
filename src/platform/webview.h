#ifndef ND_WEBVIEW_H
#define ND_WEBVIEW_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

int  nd_webview_available(void);
int  nd_webview_attach(HWND parent, const wchar_t *url, const wchar_t *user_data_dir);
void nd_webview_resize(void);
void nd_webview_reload(void);
void nd_webview_close(void);

#ifdef __cplusplus
}
#endif

#endif
