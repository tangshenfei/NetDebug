#include "httpd/server.h"
#include "httpd/api.h"
#include "core/log.h"
#include "core/thread.h"
#include "core/util.h"
#include "platform/platform.h"
#include "platform/gui.h"
#include "session/service.h"
#include "session/store.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define ND_SEP "\\"
#else
#include <signal.h>
#define ND_SEP "/"
#endif

static volatile int g_quit = 0;
static int g_stopped = 0;
static char g_token[65];

/* 有内嵌窗口宿主：Windows 走 WebView2，Linux 走 GTK + WebKitGTK */
#if defined(_WIN32) || defined(ND_HAVE_GTK)
#define ND_GUI_AVAILABLE 1
#endif

static void stop_everything(void)
{
    if (g_stopped)
        return;
    g_stopped = 1;
    ND_LOGI("shutting down");
    {
        uint64_t ids[256];
        size_t n = nd_service_list(ids, 256);
        for (size_t i = 0; i < n; i++) {
            nd_service *s = nd_service_get(ids[i]);
            if (s) {
                nd_service_stop(s);
                nd_service_put(s);
            }
        }
    }
    nd_http_stop();
}

#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    g_quit = 1;
    return TRUE;
}
#else
static void sig_handler(int sig)
{
    (void)sig;
    g_quit = 1;
}
#endif

static void make_token(void)
{
    unsigned char raw[32];
    if (nd_platform_random(raw, sizeof(raw)) != 0) {
        snprintf(g_token, sizeof(g_token), "%016llx", (unsigned long long)nd_monotonic_ms());
        return;
    }
    nd_hex_encode(raw, sizeof(raw), g_token, sizeof(g_token), 0);
}

static void write_runtime(int port)
{
    char path[600];
    cJSON *o = cJSON_CreateObject();
    char *text;

    snprintf(path, sizeof(path), "%sruntime.json", nd_config_dir());
    cJSON_AddNumberToObject(o, "port", port);
    cJSON_AddStringToObject(o, "token", g_token);
    text = cJSON_Print(o);
    if (text) {
        nd_write_file(path, text, strlen(text));
        free(text);
    }
    cJSON_Delete(o);
}

static int open_existing_instance(void)
{
    char path[600];
    void *data = NULL;
    size_t len = 0;
    cJSON *o;
    int opened = 0;

    snprintf(path, sizeof(path), "%sruntime.json", nd_config_dir());
    if (nd_read_file(path, &data, &len) != 0)
        return 0;

    o = cJSON_Parse((const char *)data);
    free(data);
    if (!o)
        return 0;

    {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "port");
        cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "token");
        if (p && cJSON_IsNumber(p) && t && cJSON_IsString(t)) {
#ifdef ND_GUI_AVAILABLE
            if (nd_gui_activate())
                opened = 1;
#endif
            printf("netdebug 已在运行：端口 %d\n", p->valueint);
        }
    }
    cJSON_Delete(o);
    return opened;
}

int main(int argc, char **argv)
{    int port = 7801;
    int open_browser = 1;
    int gui_mode = 0;
    int actual_port = 0;
    char err[256] = "";
    char url[256];
    const char *screenshot = NULL;

#ifdef ND_GUI_AVAILABLE
    gui_mode = 1;
#endif

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--no-browser") || !strcmp(argv[i], "--console")) {
            open_browser = 0;
            gui_mode = 0;
        } else if (!strcmp(argv[i], "--gui")) {
            gui_mode = 1;
        } else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (!strcmp(argv[i], "--help")) {
            printf("netdebug [--port N] [--console] [--gui]\n"
                   "  默认打开 NetDebug 主窗口；\n"
                   "  --console 不开窗口，仅命令行运行（打印调试接口地址）；\n"
                   "  --port N 指定管理端口（默认 7801）；\n"
                   "  --screenshot FILE 生成窗口截图（开发用）。\n");
            return 0;
        }
    }

    if (nd_platform_init() != 0) {
        fprintf(stderr, "network init failed\n");
        return 1;
    }

    {
        char logpath[600];
        snprintf(logpath, sizeof(logpath), "%slogs", nd_config_dir());
        nd_mkdir_p(logpath);
        snprintf(logpath, sizeof(logpath), "%slogs\\netdebug.log", nd_config_dir());
        nd_log_init(logpath);
    }
    nd_log_set_level(ND_LOG_INFO);
    ND_LOGI("netdebug 0.0.2, data dir: %s", nd_config_dir());

    if (screenshot) {
        int rc = nd_gui_capture("http://127.0.0.1:7801/#t=demo", nd_config_dir(), screenshot);
        nd_log_close();
        nd_platform_cleanup();
        return rc == 0 ? 0 : 1;
    }

    if (nd_single_instance("netdebug") == 1) {
        if (!open_existing_instance())
            printf("netdebug 已在运行，但找不到 runtime.json（可能启动未完成）\n");
        nd_platform_cleanup();
        return 0;
    }

    make_token();
    if (nd_http_start(port, g_token, &actual_port, err, sizeof(err)) != 0) {
        ND_LOGE("http start failed: %s", err);
        fprintf(stderr, "启动失败: %s\n", err);
        nd_platform_cleanup();
        return 1;
    }
    write_runtime(actual_port);
    nd_store_load_services();

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/#t=%s", actual_port, g_token);

#ifdef ND_GUI_AVAILABLE
    if (gui_mode) {
        if (nd_gui_run(url, nd_config_dir(), stop_everything) == 0) {
            stop_everything();
            nd_log_close();
            nd_platform_cleanup();
            return 0;
        }
        /* 无法创建窗口（例如没有 DISPLAY）时退回控制台模式 */
    }
#endif

    printf("netdebug 0.0.2\n");
    printf("模式: 无窗口（仅命令行）\n");
    printf("调试接口: %s\n", url);
    printf("log: %slogs" ND_SEP "netdebug.log\n", nd_config_dir());
    fflush(stdout);

    (void)open_browser; /* 不再自动拉起浏览器：界面只在 NetDebug 窗口内 */

#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
#endif

    while (!g_quit)
        nd_sleep_ms(200);

    stop_everything();
    nd_log_close();
    nd_platform_cleanup();
    return 0;
}
