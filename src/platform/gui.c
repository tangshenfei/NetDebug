#include "platform/gui.h"
#include "platform/platform.h"
#ifdef _WIN32
#include "platform/webview.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

#define ND_GUI_CLASS L"NetDebugMainWindow"
#define ND_WEBHOST_CLASS L"NetDebugWebHost"

#define ID_TITLE      2001
#define ID_SUBTITLE   2002
#define ID_STATUS     2003
#define ID_STATUS_SUB 2004
#define ID_URL_LABEL  2005
#define ID_HINT       2006
#define ID_HINT2      2009
#define ID_URL        2007
#define ID_COPY       2008

#define ID_OPEN  1001
#define ID_LOGS  1002
#define ID_STOP  1003
#define ID_BACK  1004
#define ID_STOP_PANEL 1005
#define ID_WEBERR 3001

static const char *g_url;
static const char *g_logdir;
static nd_gui_stop_fn g_stop;
static int g_dpi = 96;
static HWND g_webhost, g_web_error;
static int g_webhost_class, g_capture;

static HWND g_title, g_subtitle, g_status, g_status_sub, g_url_label;
static HWND g_hint, g_link, g_url_text, g_copy;
static WNDPROC g_url_old_proc;
static HWND g_btn_open, g_btn_logs, g_btn_stop;
static HFONT g_font_title, g_font_sub, g_font_body, g_font_small, g_font_button, g_font_link;
static HBRUSH g_bg, g_card, g_field;
static HPEN g_border;
static int g_stopping;

static int S(int v)
{
    return MulDiv(v, g_dpi, 96);
}

static int system_dpi(void)
{
    HDC dc = GetDC(NULL);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc)
        ReleaseDC(NULL, dc);
    return dpi > 0 ? dpi : 96;
}

static HICON app_icon(void)
{
    HICON icon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    return icon ? icon : LoadIcon(NULL, IDI_APPLICATION);
}

static void utf8_to_wide(const char *src, wchar_t *dst, size_t cap)
{
    if (!src || !cap)
        return;
    if (MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap) <= 0)
        dst[0] = 0;
}

static HFONT make_font(int px, int weight)
{
    return CreateFontW(px, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
}

static HWND make_text(HWND parent, const wchar_t *text, int id, int x, int y,
                      int w, int h, HFONT font)
{
    HWND c = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                             S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id,
                             NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static HFONT make_link_font(int px)
{
    return CreateFontW(px, 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
}

static void destroy_fonts(void)
{
    if (g_font_title) DeleteObject(g_font_title);
    if (g_font_sub) DeleteObject(g_font_sub);
    if (g_font_body) DeleteObject(g_font_body);
    if (g_font_small) DeleteObject(g_font_small);
    if (g_font_button) DeleteObject(g_font_button);
    if (g_font_link) DeleteObject(g_font_link);
    g_font_title = g_font_sub = g_font_body = g_font_small = g_font_button = g_font_link = NULL;
}

static void create_fonts(void)
{
    destroy_fonts();
    g_font_title = make_font(S(-26), FW_SEMIBOLD);
    g_font_sub = make_font(S(-13), FW_NORMAL);
    g_font_body = make_font(S(-15), FW_SEMIBOLD);
    g_font_small = make_font(S(-12), FW_NORMAL);
    g_font_button = make_font(S(-15), FW_SEMIBOLD);
    g_font_link = make_link_font(S(-12));
}

static void layout_controls(HWND hwnd)
{
    MoveWindow(g_title, S(96), S(26), S(420), S(32), TRUE);
    MoveWindow(g_subtitle, S(98), S(58), S(420), S(20), TRUE);
    MoveWindow(g_status, S(74), S(118), S(400), S(24), TRUE);
    MoveWindow(g_status_sub, S(74), S(142), S(460), S(20), TRUE);
    MoveWindow(g_url_label, S(52), S(172), S(200), S(18), TRUE);
    MoveWindow(g_url_text, S(62), S(192), S(400), S(24), TRUE);
    MoveWindow(g_copy, S(488), S(186), S(68), S(36), TRUE);
    MoveWindow(g_link, S(52), S(244), S(200), S(20), TRUE);
    MoveWindow(g_hint, S(250), S(244), S(330), S(20), TRUE);
    MoveWindow(g_btn_open, S(52), S(292), S(190), S(44), TRUE);
    MoveWindow(g_btn_logs, S(254), S(292), S(150), S(44), TRUE);
    MoveWindow(g_btn_stop, S(416), S(292), S(150), S(44), TRUE);
    (void)hwnd;
}

static void apply_window_style(HWND hwnd)
{
    DWORD corner = 2; /* DWMWCP_ROUND */
    BOOL dark = FALSE;
    COLORREF caption = RGB(246, 248, 251);
    COLORREF text = RGB(17, 24, 39);

    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
}

static void stop_and_close(HWND hwnd)
{
    if (g_stopping)
        return;
    g_stopping = 1;
    if (g_status) {
        SetWindowTextW(g_status, L"正在停止所有服务…");
        SetWindowTextW(g_status_sub, L"请稍候，窗口会自动关闭");
        UpdateWindow(g_status);
        UpdateWindow(g_status_sub);
    }
    if (g_stop)
        g_stop();
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK url_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SETCURSOR) {
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    }
    return CallWindowProcW(g_url_old_proc, hwnd, msg, wp, lp);
}

static void copy_url(HWND hwnd)
{
    wchar_t wurl[512];
    size_t bytes;
    HGLOBAL mem;
    utf8_to_wide(g_url, wurl, 512);
    bytes = (wcslen(wurl) + 1) * sizeof(wchar_t);
    if (!OpenClipboard(hwnd))
        return;
    EmptyClipboard();
    mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void *p = GlobalLock(mem);
        if (p) {
            memcpy(p, wurl, bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
    SetWindowTextW(g_status_sub, L"地址已复制到剪贴板");
    SetTimer(hwnd, 1, 1800, NULL);
}

static void draw_button(LPDRAWITEMSTRUCT dis)
{
    RECT rc = dis->rcItem;
    int id = (int)dis->CtlID;
    int pressed = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF fill, border, text;
    HBRUSH br;
    HPEN pen;
    HGDIOBJ oldb, oldp;
    wchar_t label[64];

    if (id == ID_OPEN) {
        fill = pressed ? RGB(29, 78, 216) : RGB(37, 99, 235);
        border = fill;
        text = RGB(255, 255, 255);
    } else if (id == ID_STOP || id == ID_STOP_PANEL) {
        fill = pressed ? RGB(254, 226, 226) : RGB(254, 242, 242);
        border = RGB(252, 165, 165);
        text = RGB(220, 38, 38);
    } else {
        fill = pressed ? RGB(226, 232, 240) : RGB(238, 242, 247);
        border = RGB(226, 232, 240);
        text = RGB(51, 65, 85);
    }

    br = CreateSolidBrush(fill);
    pen = CreatePen(PS_SOLID, 1, border);
    oldb = SelectObject(dis->hDC, br);
    oldp = SelectObject(dis->hDC, pen);
    RoundRect(dis->hDC, rc.left, rc.top, rc.right - 1, rc.bottom - 1,
              S(10), S(10));
    SelectObject(dis->hDC, oldb);
    SelectObject(dis->hDC, oldp);
    DeleteObject(br);
    DeleteObject(pen);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, text);
    SelectObject(dis->hDC, g_font_button);
    GetWindowTextW(dis->hwndItem, label, 64);
    DrawTextW(dis->hDC, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void layout_webview(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (g_webhost)
        MoveWindow(g_webhost, 0, 0, rc.right, rc.bottom, TRUE);
    if (g_web_error)
        MoveWindow(g_web_error, S(20), S(30), rc.right - S(40), rc.bottom - S(60), TRUE);
}

static LRESULT CALLBACK webhost_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) {
        nd_webview_resize();
        return 0;
    }
    if (msg == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int ensure_webhost_class(void)
{
    WNDCLASSEXW wc;
    if (g_webhost_class)
        return 1;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = webhost_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = ND_WEBHOST_CLASS;
    if (!RegisterClassExW(&wc))
        return 0;
    g_webhost_class = 1;
    return 1;
}

static void show_web_error(HWND hwnd, const wchar_t *text)
{
    if (!g_web_error) {
        g_web_error = CreateWindowExW(0, L"STATIC", text,
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      0, 0, 0, 0, hwnd,
                                      (HMENU)(INT_PTR)ID_WEBERR, NULL, NULL);
        SendMessageW(g_web_error, WM_SETFONT, (WPARAM)g_font_body, TRUE);
    } else {
        SetWindowTextW(g_web_error, text);
        ShowWindow(g_web_error, SW_SHOW);
    }
    layout_webview(hwnd);
}

static void create_webview(HWND hwnd)
{
    char dir[600];
    wchar_t wurl[1200], wdir[700];

    if (g_capture)
        return;
    if (!nd_webview_available()) {
        show_web_error(hwnd, L"系统未安装 WebView2 运行时，右侧服务面板不可用。\n"
                             L"请安装 Microsoft Edge WebView2 Runtime 后重新启动。");
        return;
    }
    if (!ensure_webhost_class()) {
        show_web_error(hwnd, L"无法创建服务面板容器。");
        return;
    }
    g_webhost = CreateWindowExW(0, ND_WEBHOST_CLASS, L"",
                                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                0, 0, 100, 100, hwnd, NULL,
                                GetModuleHandleW(NULL), NULL);
    if (!g_webhost) {
        show_web_error(hwnd, L"无法创建服务面板容器。");
        return;
    }
    layout_webview(hwnd);

    utf8_to_wide(g_url, wurl, 1200);
    snprintf(dir, sizeof(dir), "%swebview", nd_config_dir());
    nd_mkdir_p(dir);
    utf8_to_wide(dir, wdir, 700);
    if (nd_webview_attach(g_webhost, wurl, wdir) != 0) {
        DestroyWindow(g_webhost);
        g_webhost = NULL;
        show_web_error(hwnd, L"WebView2 初始化失败，右侧服务面板不可用。");
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DRAWITEM:
        if (lp) {
            draw_button((LPDRAWITEMSTRUCT)lp);
            return TRUE;
        }
        break;
    case WM_CREATE:
        g_dpi = (int)GetDpiForWindow(hwnd);
        if (g_dpi <= 0)
            g_dpi = 96;
        create_fonts();
        g_bg = CreateSolidBrush(RGB(246, 248, 251));
        g_card = CreateSolidBrush(RGB(255, 255, 255));
        g_field = CreateSolidBrush(RGB(242, 245, 249));
        g_border = CreatePen(PS_SOLID, 1, RGB(227, 232, 239));
        create_webview(hwnd);
        return 0;

    case WM_SIZE:
        layout_webview(hwnd);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, g_bg);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        int id = GetDlgCtrlID((HWND)lp);
        SetBkMode(dc, TRANSPARENT);
        if (id == ID_TITLE) {
            SetTextColor(dc, RGB(17, 24, 39));
            return (LRESULT)g_bg;
        }
        if (id == ID_SUBTITLE) {
            SetTextColor(dc, RGB(107, 114, 128));
            return (LRESULT)g_bg;
        }
        if (id == ID_STATUS) {
            SetTextColor(dc, RGB(17, 24, 39));
            return (LRESULT)g_card;
        }
        if (id == ID_STATUS_SUB || id == ID_URL_LABEL || id == ID_HINT2) {
            SetTextColor(dc, RGB(107, 114, 128));
            return (LRESULT)g_card;
        }
        if (id == ID_HINT) {
            SetTextColor(dc, RGB(37, 99, 235));
            return (LRESULT)g_card;
        }
        if (id == ID_URL) {
            SetTextColor(dc, RGB(37, 99, 235));
            return (LRESULT)g_field;
        }
        return (LRESULT)g_card;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(55, 65, 81));
        return (LRESULT)g_field;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_HINT:
        case ID_URL:
        case ID_OPEN:
            if (g_webhost)
                nd_webview_reload();
            else
                create_webview(hwnd);
            return 0;
        case ID_LOGS: {
            wchar_t wdir[600];
            utf8_to_wide(g_logdir, wdir, 600);
            ShellExecuteW(NULL, L"open", wdir, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        case ID_COPY:
            copy_url(hwnd);
            return 0;
        case ID_STOP:
            stop_and_close(hwnd);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hwnd, 1);
            SetWindowTextW(g_status_sub, L"管理页面只监听本机回环地址");
        }
        return 0;

    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        g_dpi = HIWORD(wp);
        create_fonts();
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left,
                     r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout_webview(hwnd);
        if (g_web_error)
            SendMessageW(g_web_error, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_CLOSE:
        stop_and_close(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        nd_webview_close();
        g_webhost = NULL;
        g_web_error = NULL;
        destroy_fonts();
        if (g_bg) DeleteObject(g_bg);
        if (g_card) DeleteObject(g_card);
        if (g_field) DeleteObject(g_field);
        if (g_border) DeleteObject(g_border);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int nd_gui_activate(void)
{
    HWND hwnd = FindWindowW(ND_GUI_CLASS, NULL);
    if (!hwnd)
        return 0;
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    return 1;
}

int nd_gui_run(const char *url, const char *log_dir, nd_gui_stop_fn stop)
{
    WNDCLASSEXW wc;
    HWND hwnd, con;
    MSG msg;
    RECT rc, wa;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                  WS_THICKFRAME | WS_MAXIMIZEBOX;
    int w, h, x, y;
    int own_console = 0;

    g_url = url;
    g_logdir = log_dir;
    g_stop = stop;

    g_dpi = system_dpi();

    con = GetConsoleWindow();
    if (con) {
        DWORD pids[2];
        if (GetConsoleProcessList(pids, 2) <= 1) {
            own_console = 1;
            ShowWindow(con, SW_HIDE);
        }
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = app_icon();
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = NULL;
    wc.lpszClassName = ND_GUI_CLASS;
    if (!RegisterClassExW(&wc)) {
        if (own_console)
            ShowWindow(con, SW_SHOW);
        return -1;
    }

    rc.left = 0;
    rc.top = 0;
    rc.right = S(1200);
    rc.bottom = S(820);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (wa.right - wa.left > S(900) && rc.right > wa.right - wa.left - S(40))
        rc.right = wa.right - wa.left - S(40);
    if (wa.bottom - wa.top > S(600) && rc.bottom > wa.bottom - wa.top - S(40))
        rc.bottom = wa.bottom - wa.top - S(40);
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    x = wa.left + (wa.right - wa.left - w) / 2;
    y = wa.top + (wa.bottom - wa.top - h) / 2;

    hwnd = CreateWindowExW(0, ND_GUI_CLASS, L"NetDebug", style,
                           x, y, w, h, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        if (own_console)
            ShowWindow(con, SW_SHOW);
        MessageBoxW(NULL, L"创建控制窗口失败，请使用 --console 以控制台方式运行。",
                    L"NetDebug", MB_OK | MB_ICONERROR);
        return -1;
    }

    apply_window_style(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (con)
        FreeConsole();
    (void)freopen("NUL", "w", stdout);
    (void)freopen("NUL", "w", stderr);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

int nd_gui_capture(const char *url, const char *log_dir, const char *bmp_path)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    RECT wr;
    HDC screen, mem;
    HBITMAP bmp;
    BITMAPINFO bi;
    BITMAPFILEHEADER fh;
    void *bits;
    int w, h;
    FILE *fp;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    g_url = url;
    g_logdir = log_dir;
    g_stop = NULL;
    g_dpi = system_dpi();
    g_capture = 1;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = app_icon();
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = ND_GUI_CLASS;
    if (!RegisterClassExW(&wc))
        return -1;

    hwnd = CreateWindowExW(0, ND_GUI_CLASS, L"NetDebug", style,
                           100, 100, S(636), S(431), NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
        return -1;
    apply_window_style(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    Sleep(250);

    GetWindowRect(hwnd, &wr);
    w = wr.right - wr.left;
    h = wr.bottom - wr.top;
    screen = GetDC(NULL);
    mem = CreateCompatibleDC(screen);
    bmp = CreateCompatibleBitmap(screen, w, h);
    SelectObject(mem, bmp);
    if (!PrintWindow(hwnd, mem, 2))
        BitBlt(mem, 0, 0, w, h, screen, wr.left, wr.top, SRCCOPY);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bits = malloc((size_t)w * h * 4);
    GetDIBits(mem, bmp, 0, (UINT)h, bits, &bi, DIB_RGB_COLORS);

    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + (DWORD)((size_t)w * h * 4);

    fp = fopen(bmp_path, "wb");
    if (fp) {
        fwrite(&fh, sizeof(fh), 1, fp);
        fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, fp);
        fwrite(bits, (size_t)w * h * 4, 1, fp);
        fclose(fp);
    }
    free(bits);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    DestroyWindow(hwnd);
    return fp ? 0 : -1;
}

#elif !defined(ND_HAVE_GTK)

int nd_gui_run(const char *url, const char *log_dir, nd_gui_stop_fn stop)
{
    (void)url;
    (void)log_dir;
    (void)stop;
    return -1;
}

int nd_gui_capture(const char *url, const char *log_dir, const char *bmp_path)
{
    (void)url;
    (void)log_dir;
    (void)bmp_path;
    return -1;
}

#endif
