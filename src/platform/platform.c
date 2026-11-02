#include "platform/platform.h"
#include "core/log.h"
#include "core/util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <bcrypt.h>
#include <direct.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <unistd.h>
#endif

static char g_conf_dir[512];
static int  g_data_fallback = 0;

int nd_platform_init(void)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return -1;
#endif
    return 0;
}

void nd_platform_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

#ifdef _WIN32

uint64_t nd_wall_ms(void)
{
    FILETIME ft;
    ULARGE_INTEGER li;
    GetSystemTimeAsFileTime(&ft);
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    return (li.QuadPart - 116444736000000000ULL) / 10000ULL;
}

int nd_platform_random(void *buf, size_t n)
{
    NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return st == 0 ? 0 : -1;
}

static HANDLE g_instance = NULL;

int nd_single_instance(const char *name)
{
    char mutex_name[256];
    snprintf(mutex_name, sizeof(mutex_name), "Local\\nd_%s", name ? name : "netdebug");
    g_instance = CreateMutexA(NULL, FALSE, mutex_name);
    if (!g_instance)
        return -1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_instance);
        g_instance = NULL;
        return 1;
    }
    return 0;
}

static int dir_writable(const char *dir)
{
    char probe[700];
    FILE *f;
    snprintf(probe, sizeof(probe), "%s.nd-probe-%lu", dir,
             (unsigned long)GetCurrentProcessId());
    f = fopen(probe, "wb");
    if (!f)
        return 0;
    fclose(f);
    DeleteFileA(probe);
    return 1;
}

static void exe_dir(char *out, size_t cap)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    char *slash;
    if (n == 0 || n >= sizeof(path)) {
        nd_strlcpy(out, ".", cap);
        return;
    }
    path[n] = '\0';
    slash = strrchr(path, '\\');
    if (slash)
        *slash = '\0';
    nd_strlcpy(out, path[0] ? path : ".", cap);
}

static void dir_with_sep(char *out, size_t cap, const char *base, const char *leaf)
{
    size_t n;
    if (leaf)
        snprintf(out, cap, "%s%s%s", base,
                 (base[0] && (base[strlen(base) - 1] == '\\' || base[strlen(base) - 1] == '/')) ? "" : "\\",
                 leaf);
    else
        nd_strlcpy(out, base, cap);
    n = strlen(out);
    if (n && out[n - 1] != '\\' && out[n - 1] != '/')
        snprintf(out + n, cap - n, "\\");
}

const char *nd_config_dir(void)
{
    const char *home;
    const char *appdata;
    const char *local;
    char cand[5][700];
    char exe[600], cwd[600];
    int n = 0;
    int chosen = -1;

    if (g_conf_dir[0])
        return g_conf_dir;

    home = getenv("NETDEBUG_HOME");
    if (home && home[0])
        dir_with_sep(cand[n++], sizeof(cand[0]), home, NULL);

    appdata = getenv("APPDATA");
    if (appdata && appdata[0])
        dir_with_sep(cand[n++], sizeof(cand[0]), appdata, "NetDebug");
    local = getenv("LOCALAPPDATA");
    if (local && local[0])
        dir_with_sep(cand[n++], sizeof(cand[0]), local, "NetDebug");

    exe_dir(exe, sizeof(exe));
    dir_with_sep(cand[n++], sizeof(cand[0]), exe,
                 "NetDebug-data");
    if (GetCurrentDirectoryA((DWORD)sizeof(cwd), cwd))
        dir_with_sep(cand[n++], sizeof(cand[0]), cwd, ".netdebug");
    else
        dir_with_sep(cand[n++], sizeof(cand[0]), ".", ".netdebug");

    for (int i = 0; i < n; i++) {
        nd_mkdir_p(cand[i]);
        if (dir_writable(cand[i])) {
            chosen = i;
            break;
        }
        ND_LOGW("config dir not writable, skipping: %s", cand[i]);
    }
    if (chosen < 0)
        chosen = 0;

    nd_strlcpy(g_conf_dir, cand[chosen], sizeof(g_conf_dir));
    g_data_fallback = chosen != 0;
    ND_LOGI("data dir: %s%s", g_conf_dir, chosen ? " (fallback)" : "");
    {
        size_t gl = strlen(g_conf_dir);
        if (gl && g_conf_dir[gl - 1] != '\\' && gl + 1 < sizeof(g_conf_dir)) {
            g_conf_dir[gl] = '\\';
            g_conf_dir[gl + 1] = '\0';
        }
    }
    if (chosen != 0) {
        fprintf(stderr, "note: %s is not writable; using %s\n",
                cand[0], g_conf_dir);
    }
    return g_conf_dir;
}

int nd_mkdir_p(const char *path)
{
    char tmp[512];
    size_t i, len;
    if (!path)
        return -1;
    len = nd_strlcpy(tmp, path, sizeof(tmp));
    if (len >= sizeof(tmp))
        return -1;
    for (i = 0; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char save = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] && !(strlen(tmp) == 2 && tmp[1] == ':'))
                _mkdir(tmp);
            tmp[i] = save;
        }
    }
    _mkdir(tmp);
    return 0;
}

char *nd_sys_error(int err, char *buf, size_t cap)
{
    WCHAR wbuf[512];
    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD n = FormatMessageW(flags, NULL, (DWORD)err, 0, wbuf,
                             (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])), NULL);
    if (!n) {
        snprintf(buf, cap, "error %d", err);
        return buf;
    }
    while (n > 0 && (wbuf[n - 1] == L'\r' || wbuf[n - 1] == L'\n' || wbuf[n - 1] == L' '))
        wbuf[--n] = L'\0';
    if (WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, (int)cap, NULL, NULL) <= 0)
        snprintf(buf, cap, "error %d", err);
    return buf;
}

int nd_pick_folder(char *out, size_t cap)
{
    HRESULT init_hr;
    HRESULT hr;
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    PWSTR wpath = NULL;
    HWND owner;

    if (!out || cap == 0)
        return -1;
    out[0] = '\0';
    owner = FindWindowW(L"NetDebugMainWindow", NULL);
    if (!owner)
        owner = GetForegroundWindow();

    init_hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(init_hr)) {
        hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IFileOpenDialog, (void **)&dlg);
        if (SUCCEEDED(hr) && dlg) {
            DWORD opts = 0;
            dlg->lpVtbl->GetOptions(dlg, &opts);
            dlg->lpVtbl->SetOptions(dlg, opts | FOS_PICKFOLDERS |
                                          FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            hr = dlg->lpVtbl->Show(dlg, owner);
            if (hr == S_OK && SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item)) && item) {
                if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &wpath)) &&
                    wpath) {
                    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, out, (int)cap, NULL, NULL);
                    out[cap - 1] = '\0';
                    CoTaskMemFree(wpath);
                    item->lpVtbl->Release(item);
                    dlg->lpVtbl->Release(dlg);
                    CoUninitialize();
                    return 1;
                }
            }
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                dlg->lpVtbl->Release(dlg);
                CoUninitialize();
                return 0;
            }
        }
    }

    if (item)
        item->lpVtbl->Release(item);
    if (dlg)
        dlg->lpVtbl->Release(dlg);
    if (SUCCEEDED(init_hr))
        CoUninitialize();

    /* 老系统/COM 不可用时的兜底 */
    {
        BROWSEINFOW bi;
        LPITEMIDLIST pidl;
        wchar_t wdir[MAX_PATH];
        memset(&bi, 0, sizeof(bi));
        bi.hwndOwner = owner;
        bi.lpszTitle = L"选择目录";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        pidl = SHBrowseForFolderW(&bi);
        if (!pidl)
            return 0;
        if (SHGetPathFromIDListW(pidl, wdir)) {
            WideCharToMultiByte(CP_UTF8, 0, wdir, -1, out, (int)cap, NULL, NULL);
            out[cap - 1] = '\0';
            CoTaskMemFree(pidl);
            return 1;
        }
        CoTaskMemFree(pidl);
    }
    return -1;
}

#else /* POSIX */

uint64_t nd_wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

int nd_platform_random(void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom((char *)buf + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        got += (size_t)r;
    }
    if (got == n)
        return 0;
    {
        FILE *f = fopen("/dev/urandom", "rb");
        if (!f)
            return -1;
        got = fread(buf, 1, n, f);
        fclose(f);
    }
    return got == n ? 0 : -1;
}

static int g_lock_fd = -1;

int nd_single_instance(const char *name)
{
    char path[600];
    snprintf(path, sizeof(path), "%s%s.lock", nd_config_dir(), name ? name : "netdebug");
    g_lock_fd = open(path, O_CREAT | O_RDWR, 0600);
    if (g_lock_fd < 0)
        return -1;
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(g_lock_fd);
        g_lock_fd = -1;
        return 1;
    }
    return 0;
}

static int posix_writable_dir(const char *dir)
{
    char probe[700];
    FILE *f;
    snprintf(probe, sizeof(probe), "%s.nd-probe-%ld", dir, (long)getpid());
    f = fopen(probe, "wb");
    if (!f)
        return 0;
    fclose(f);
    unlink(probe);
    return 1;
}

static void posix_dir(char *out, size_t cap, const char *base, const char *leaf)
{
    size_t n;
    if (leaf)
        snprintf(out, cap, "%s%s%s", base,
                 (base[0] && base[strlen(base) - 1] == '/') ? "" : "/", leaf);
    else
        nd_strlcpy(out, base, cap);
    n = strlen(out);
    if (n && out[n - 1] != '/')
        snprintf(out + n, cap - n, "/");
}

const char *nd_config_dir(void)
{
    const char *xdg, *home, *env;
    char cand[4][700];
    char cwd[600];
    int n = 0;
    int chosen = -1;

    if (g_conf_dir[0])
        return g_conf_dir;

    env = getenv("NETDEBUG_HOME");
    if (env && env[0])
        posix_dir(cand[n++], sizeof(cand[0]), env, NULL);

    xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        posix_dir(cand[n++], sizeof(cand[0]), xdg, "netdebug");

    home = getenv("HOME");
    if (home && home[0])
        posix_dir(cand[n++], sizeof(cand[0]), home, ".config/netdebug");

    if (getcwd(cwd, sizeof(cwd)))
        posix_dir(cand[n++], sizeof(cand[0]), cwd, ".netdebug");
    else
        posix_dir(cand[n++], sizeof(cand[0]), ".", ".netdebug");

    for (int i = 0; i < n; i++) {
        nd_mkdir_p(cand[i]);
        if (posix_writable_dir(cand[i])) {
            chosen = i;
            break;
        }
    }
    if (chosen < 0)
        chosen = 0;

    nd_strlcpy(g_conf_dir, cand[chosen], sizeof(g_conf_dir));
    g_data_fallback = chosen != 0;
    {
        size_t gl = strlen(g_conf_dir);
        if (gl && g_conf_dir[gl - 1] != '/' && gl + 1 < sizeof(g_conf_dir)) {
            g_conf_dir[gl] = '/';
            g_conf_dir[gl + 1] = '\0';
        }
    }
    if (chosen != 0)
        fprintf(stderr, "note: %s is not writable; using %s\n", cand[0], g_conf_dir);
    return g_conf_dir;
}

int nd_mkdir_p(const char *path)
{
    char tmp[512];
    size_t i, len;
    if (!path)
        return -1;
    len = nd_strlcpy(tmp, path, sizeof(tmp));
    if (len >= sizeof(tmp))
        return -1;
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0700);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0700);
    return 0;
}

char *nd_sys_error(int err, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s", strerror(err));
    return buf;
}

int nd_pick_folder(char *out, size_t cap)
{
    (void)out;
    (void)cap;
    return -1;
}

#endif

int nd_path_join(char *out, size_t cap, const char *a, const char *b)
{
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    size_t la = a ? strlen(a) : 0;
    if (!a || !b)
        return -1;
    if (la > 0 && (a[la - 1] == '/' || a[la - 1] == '\\'))
        return snprintf(out, cap, "%s%s", a, b) < (int)cap ? 0 : -1;
    return snprintf(out, cap, "%s%c%s", a, sep, b) < (int)cap ? 0 : -1;
}

int nd_write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    size_t n;
    if (!f)
        return -1;
    n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

int nd_read_file(const char *path, void **out, size_t *len)
{
    FILE *f;
    long sz;
    void *buf;
    size_t n;

    *out = NULL;
    *len = 0;
    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);
    buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return -1;
    }
    ((char *)buf)[sz] = '\0';
    *out = buf;
    *len = n;
    return 0;
}
