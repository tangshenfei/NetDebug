#ifndef ND_LOG_H
#define ND_LOG_H

typedef enum {
    ND_LOG_DEBUG = 0,
    ND_LOG_INFO  = 1,
    ND_LOG_WARN  = 2,
    ND_LOG_ERROR = 3
} nd_log_level;

void nd_log_init(const char *path);
void nd_log_close(void);
void nd_log_set_level(int level);
void nd_log_msg(int level, const char *file, int line, const char *fmt, ...);

#define ND_LOGD(...) nd_log_msg(ND_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define ND_LOGI(...) nd_log_msg(ND_LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define ND_LOGW(...) nd_log_msg(ND_LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define ND_LOGE(...) nd_log_msg(ND_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif /* ND_LOG_H */
