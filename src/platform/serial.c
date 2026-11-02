#include "platform/serial.h"
#include "platform/platform.h"
#include "core/log.h"
#include "core/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>

struct nd_serial {
    HANDLE h;
    char   name[64];
    volatile LONG closing;
};

static void wide_to_utf8(const WCHAR *w, char *out, size_t cap)
{
    int n;
    if (!w || !*w) {
        if (cap) out[0] = '\0';
        return;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    if (n <= 0 && cap)
        out[0] = '\0';
}

static int add_port(nd_serial_port *out, size_t cap, size_t *n,
                    const char *port, const char *desc)
{
    if (!port || !port[0] || *n >= cap)
        return 0;
    for (size_t i = 0; i < *n; i++) {
        if (!strcmp(out[i].port, port))
            return 0;
    }
    nd_strlcpy(out[*n].port, port, sizeof(out[*n].port));
    nd_strlcpy(out[*n].desc, desc && desc[0] ? desc : "串口设备",
               sizeof(out[*n].desc));
    (*n)++;
    return 1;
}

int nd_serial_list(nd_serial_port *out, size_t cap, size_t *n)
{
    HDEVINFO set;
    SP_DEVINFO_DATA data;
    DWORD i;
    size_t count = 0;

    *n = 0;
    set = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (set != INVALID_HANDLE_VALUE) {
        data.cbSize = sizeof(data);
        for (i = 0; SetupDiEnumDeviceInfo(set, i, &data); i++) {
            WCHAR friendly[256];
            char desc[128] = "";
            char port[64] = "";
            DWORD size = 0;
            if (!SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_FRIENDLYNAME,
                                                   NULL, (PBYTE)friendly,
                                                   sizeof(friendly), &size))
                continue;
            wide_to_utf8(friendly, desc, sizeof(desc));
            {
                char *lp = strstr(desc, "(COM");
                if (lp) {
                    char *rp = strchr(lp, ')');
                    if (rp) {
                        size_t len = (size_t)(rp - lp - 1);
                        if (len < sizeof(port)) {
                            memcpy(port, lp + 1, len);
                            port[len] = '\0';
                        }
                    }
                }
            }
            if (port[0])
                add_port(out, cap, &count, port, desc);
        }
        SetupDiDestroyDeviceInfoList(set);
    }

    /* 注册表兜底：没有友好名的设备 */
    {
        HKEY key;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                          0, KEY_READ, &key) == ERROR_SUCCESS) {
            for (DWORD idx = 0;; idx++) {
                char name[256];
                char value[64];
                DWORD nlen = sizeof(name);
                DWORD vlen = sizeof(value);
                DWORD type = 0;
                LONG rc = RegEnumValueA(key, idx, name, &nlen, NULL, &type,
                                        (BYTE *)value, &vlen);
                if (rc != ERROR_SUCCESS)
                    break;
                value[sizeof(value) - 1] = '\0';
                if (type == REG_SZ && value[0])
                    add_port(out, cap, &count, value, "串口设备");
            }
            RegCloseKey(key);
        }
    }

    *n = count;
    return 0;
}

static int map_parity(int parity)
{
    switch (parity) {
    case 1: return ODDPARITY;
    case 2: return EVENPARITY;
    case 3: return MARKPARITY;
    case 4: return SPACEPARITY;
    default: return NOPARITY;
    }
}

nd_serial *nd_serial_open(const char *port, int baud, int data_bits,
                          int parity, int stop_bits, int flow,
                          int dtr, int rts, char *err, size_t errlen)
{
    char path[128];
    HANDLE h;
    DCB dcb;
    COMMTIMEOUTS to;
    nd_serial *s;

    if (!port || !port[0]) {
        if (err) snprintf(err, errlen, "串口名称为空");
        return NULL;
    }
    snprintf(path, sizeof(path), "\\\\.\\%s", port);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) {
            char sys[256];
            snprintf(err, errlen, "打开 %s 失败: %s", port,
                     nd_sys_error((int)GetLastError(), sys, sizeof(sys)));
        }
        return NULL;
    }

    SetupComm(h, 64 * 1024, 64 * 1024);
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        if (err) snprintf(err, errlen, "读取串口参数失败");
        CloseHandle(h);
        return NULL;
    }
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = (BYTE)data_bits;
    dcb.Parity = (BYTE)map_parity(parity);
    dcb.fBinary = TRUE;
    dcb.fParity = parity != 0;
    dcb.fOutxCtsFlow = (flow == 1);
    dcb.fRtsControl = (flow == 1) ? RTS_CONTROL_HANDSHAKE
                                  : (rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE);
    dcb.fOutX = (flow == 2);
    dcb.fInX = (flow == 2);
    dcb.XonChar = 0x11;
    dcb.XoffChar = 0x13;
    dcb.fDtrControl = dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    dcb.StopBits = (stop_bits == 2) ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fAbortOnError = FALSE;

    if (!SetCommState(h, &dcb)) {
        if (err) {
            char sys[256];
            snprintf(err, errlen, "设置串口参数失败: %s",
                     nd_sys_error((int)GetLastError(), sys, sizeof(sys)));
        }
        CloseHandle(h);
        return NULL;
    }

    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = 100;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 5000;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    if (dtr)
        EscapeCommFunction(h, SETDTR);
    if (rts && flow != 1)
        EscapeCommFunction(h, SETRTS);

    s = (nd_serial *)calloc(1, sizeof(*s));
    if (!s) {
        CloseHandle(h);
        return NULL;
    }
    s->h = h;
    nd_strlcpy(s->name, port, sizeof(s->name));
    return s;
}

void nd_serial_cancel(nd_serial *s)
{
    if (!s)
        return;
    InterlockedExchange(&s->closing, 1);
    if (s->h && s->h != INVALID_HANDLE_VALUE)
        CancelIoEx(s->h, NULL);
}

void nd_serial_close(nd_serial *s)
{
    if (!s)
        return;
    nd_serial_cancel(s);
    if (s->h && s->h != INVALID_HANDLE_VALUE) {
        CloseHandle(s->h);
        s->h = INVALID_HANDLE_VALUE;
    }
    free(s);
}

int nd_serial_read(nd_serial *s, void *buf, size_t cap, int timeout_ms)
{
    DWORD start, got = 0;

    if (!s || s->h == INVALID_HANDLE_VALUE)
        return -1;
    start = GetTickCount();
    for (;;) {
        if (InterlockedCompareExchange(&s->closing, 0, 0))
            return -1;
        if (!ReadFile(s->h, buf, (DWORD)cap, &got, NULL))
            return -1;
        if (got > 0)
            return (int)got;
        if ((int)(GetTickCount() - start) >= timeout_ms)
            return -2;
        Sleep(5);
    }
}

int nd_serial_write(nd_serial *s, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t off = 0;

    if (!s || s->h == INVALID_HANDLE_VALUE)
        return -1;
    while (off < len) {
        DWORD wrote = 0;
        if (!WriteFile(s->h, p + off, (DWORD)(len - off), &wrote, NULL))
            return -1;
        if (wrote == 0)
            return -1;
        off += wrote;
    }
    return 0;
}

int nd_serial_set_dtr(nd_serial *s, int on)
{
    if (!s || s->h == INVALID_HANDLE_VALUE)
        return -1;
    return EscapeCommFunction(s->h, on ? SETDTR : CLRDTR) ? 0 : -1;
}

int nd_serial_set_rts(nd_serial *s, int on)
{
    if (!s || s->h == INVALID_HANDLE_VALUE)
        return -1;
    return EscapeCommFunction(s->h, on ? SETRTS : CLRRTS) ? 0 : -1;
}

const char *nd_serial_port_name(nd_serial *s)
{
    return s ? s->name : "";
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

struct nd_serial {
    int  fd;
    char name[64];
};

static int add_port(nd_serial_port *out, size_t cap, size_t *n, const char *path)
{
    if (*n >= cap)
        return 0;
    for (size_t i = 0; i < *n; i++)
        if (!strcmp(out[i].port, path))
            return 0;
    nd_strlcpy(out[*n].port, path, sizeof(out[*n].port));
    nd_strlcpy(out[*n].desc, path, sizeof(out[*n].desc));
    (*n)++;
    return 1;
}

int nd_serial_list(nd_serial_port *out, size_t cap, size_t *n)
{
    static const char *patterns[] = {
        "/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyS*", "/dev/serial/by-id/*"
    };
    size_t count = 0;

    *n = 0;
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        glob_t g;
        if (glob(patterns[i], 0, NULL, &g) != 0)
            continue;
        for (size_t k = 0; k < g.gl_pathc; k++)
            add_port(out, cap, &count, g.gl_pathv[k]);
        globfree(&g);
    }
    *n = count;
    return 0;
}

static speed_t map_baud(int baud)
{
    switch (baud) {
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
#ifdef B230400
    case 230400: return B230400;
#endif
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: return 0;
    }
}

nd_serial *nd_serial_open(const char *port, int baud, int data_bits,
                          int parity, int stop_bits, int flow,
                          int dtr, int rts, char *err, size_t errlen)
{
    int fd;
    struct termios tio;
    speed_t sp;
    nd_serial *s;

    sp = map_baud(baud);
    if (sp == 0) {
        if (err) snprintf(err, errlen, "不支持的波特率: %d", baud);
        return NULL;
    }
    fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        if (err) snprintf(err, errlen, "打开 %s 失败: %s", port, strerror(errno));
        return NULL;
    }
    if (tcgetattr(fd, &tio) != 0) {
        if (err) snprintf(err, errlen, "读取串口参数失败: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CSIZE;
    switch (data_bits) {
    case 5: tio.c_cflag |= CS5; break;
    case 6: tio.c_cflag |= CS6; break;
    case 7: tio.c_cflag |= CS7; break;
    default: tio.c_cflag |= CS8; break;
    }
    if (parity) {
        tio.c_cflag |= PARENB;
        if (parity == 1 || parity == 3)
            tio.c_cflag |= PARODD;
    } else {
        tio.c_cflag &= ~PARENB;
    }
    if (stop_bits == 2)
        tio.c_cflag |= CSTOPB;
    else
        tio.c_cflag &= ~CSTOPB;
#ifdef CRTSCTS
    if (flow == 1)
        tio.c_cflag |= CRTSCTS;
    else
        tio.c_cflag &= ~CRTSCTS;
#endif
    if (flow == 2) {
        tio.c_iflag |= (IXON | IXOFF);
    } else {
        tio.c_iflag &= ~(IXON | IXOFF);
    }
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        if (err) snprintf(err, errlen, "设置串口参数失败: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    {
        int bits = 0;
        if (dtr) bits |= TIOCM_DTR;
        if (rts) bits |= TIOCM_RTS;
        if (bits)
            ioctl(fd, TIOCMBIS, &bits);
        else {
            bits = TIOCM_DTR | TIOCM_RTS;
            ioctl(fd, TIOCMBIC, &bits);
        }
    }

    s = (nd_serial *)calloc(1, sizeof(*s));
    if (!s) {
        close(fd);
        return NULL;
    }
    s->fd = fd;
    nd_strlcpy(s->name, port, sizeof(s->name));
    return s;
}

void nd_serial_cancel(nd_serial *s)
{
    (void)s; /* poll 带超时，关闭前不会永久阻塞 */
}

void nd_serial_close(nd_serial *s)
{
    if (!s)
        return;
    if (s->fd >= 0)
        close(s->fd);
    free(s);
}

int nd_serial_read(nd_serial *s, void *buf, size_t cap, int timeout_ms)
{
    struct pollfd p;
    int rc;
    ssize_t n;

    if (!s || s->fd < 0)
        return -1;
    p.fd = s->fd;
    p.events = POLLIN;
    p.revents = 0;
    rc = poll(&p, 1, timeout_ms);
    if (rc == 0)
        return -2;
    if (rc < 0)
        return -1;
    n = read(s->fd, buf, cap);
    if (n > 0)
        return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return -2;
    return -1;
}

int nd_serial_write(nd_serial *s, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t off = 0;

    if (!s || s->fd < 0)
        return -1;
    while (off < len) {
        ssize_t n = write(s->fd, p + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd pfd;
            pfd.fd = s->fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, 1000) <= 0)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

int nd_serial_set_dtr(nd_serial *s, int on)
{
    int bits = TIOCM_DTR;
    if (!s || s->fd < 0)
        return -1;
    return ioctl(s->fd, on ? TIOCMBIS : TIOCMBIC, &bits);
}

int nd_serial_set_rts(nd_serial *s, int on)
{
    int bits = TIOCM_RTS;
    if (!s || s->fd < 0)
        return -1;
    return ioctl(s->fd, on ? TIOCMBIS : TIOCMBIC, &bits);
}

const char *nd_serial_port_name(nd_serial *s)
{
    return s ? s->name : "";
}

#endif
