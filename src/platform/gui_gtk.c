#include "platform/gui.h"

#include "core/log.h"

#ifdef ND_HAVE_GTK

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <glib-unix.h>

#include <signal.h>
#include <stdlib.h>

static nd_gui_stop_fn g_stop;
static GtkWidget *g_window;

/* Ctrl+C / SIGTERM：停下来并退出 GTK 主循环 */
static gboolean on_unix_signal(gpointer data)
{
    (void)data;
    if (g_stop)
        g_stop();
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

static void on_window_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    g_window = NULL;
    gtk_main_quit();
}

/* 关窗等同于「停止全部服务并退出」 */
static gboolean on_delete_event(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)w;
    (void)ev;
    (void)data;
    if (g_stop)
        g_stop();
    return FALSE; /* 继续销毁窗口 */
}

static void on_load_failed(WebKitWebView *view, WebKitLoadEvent ev,
                           const char *uri, GError *err, gpointer data)
{
    (void)view;
    (void)ev;
    (void)uri;
    (void)data;
    ND_LOGE("page load failed: %s", err && err->message ? err->message : "unknown");
}

int nd_gui_run(const char *url, const char *log_dir, nd_gui_stop_fn stop)
{
    GtkWidget *web;
    char title[256];

    (void)log_dir;
    g_stop = stop;

    if (!url)
        return -1;
    if (!gtk_init_check(NULL, NULL)) {
        ND_LOGW("no display available, falling back to console mode");
        return -1;
    }

    snprintf(title, sizeof(title), "NetDebug · 网络调试工作台");

    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), title);
    gtk_window_set_default_size(GTK_WINDOW(g_window), 1200, 820);
    gtk_window_set_icon_name(GTK_WINDOW(g_window), "network-wired");
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_window_destroy), NULL);
    g_signal_connect(g_window, "delete-event", G_CALLBACK(on_delete_event), NULL);

    web = webkit_web_view_new();
    g_signal_connect(web, "load-failed", G_CALLBACK(on_load_failed), NULL);
    gtk_container_add(GTK_CONTAINER(g_window), web);
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(web), url);
    gtk_widget_show_all(g_window);
    ND_LOGI("main window created, loading %s", url);

    g_unix_signal_add(SIGINT, on_unix_signal, NULL);
    g_unix_signal_add(SIGTERM, on_unix_signal, NULL);

    gtk_main();

    if (g_stop)
        g_stop();
    return 0;
}

/* Linux 侧没有跨进程激活窗口的需求：单实例时直接提示即可 */
int nd_gui_activate(void)
{
    return 0;
}

int nd_gui_capture(const char *url, const char *log_dir, const char *out_path)
{
    (void)url;
    (void)log_dir;
    (void)out_path;
    return -1;
}

#endif /* ND_HAVE_GTK */
